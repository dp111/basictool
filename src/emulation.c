#include "emulation.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "driver.h"
#include "lib6502.h"
#include "roms.h"
#include "utils.h"

static M6502_Registers mpu_registers;
M6502_Memory mpu_memory;
static M6502_Callbacks mpu_callbacks;
static M6502 *mpu;

// M6502_run() never returns, so we use this jmp_buf to return control when
// the emulated machine is waiting for user input.
static jmp_buf mpu_env;

static int vdu_variables[256];

static enum {
    ms_running,
    ms_osword_input_line_pending,
    ms_osrdch_pending,
} mpu_state = ms_running;

// We copy transient bits of machine code to transient_code for execution; such
// code must not JSR to anything which could in turn overwrite transient_code,
// as the code following the JSR might have been overwritten when it returned.
static const int transient_code = 0x900;
// The code to invoke the ROM service handler can't live at transient_code as
// it needs to JSR into arbitrary ROM code, so it has its own space.
static const int service_code = 0xb00;

enum {
    os_text_pointer = 0xf2,
    romsel_copy = 0xf4,
    brkv = 0x202,
    wrchv = 0x20e,
    fake_irq_handler = 0xf000,
    oswrch = 0xffee
};

static void mpu_write_u16(uint16_t address, uint16_t data) {
    check(address != 0xffff, "internal error: write_u16 at top of memory");
    mpu_memory[address    ] = data & 0xff;
    mpu_memory[address + 1] = (data >> 8) & 0xff;
}

uint16_t mpu_read_u16(uint16_t address) {
    check(address != 0xffff, "internal error: read_u16 at top of memory");
    return (mpu_memory[address + 1] << 8) | mpu_memory[address];
}

static void mpu_clear_carry(void) {
    mpu_registers.p &= ~(1<<0);
}

static void mpu_dump(void) {
    char buffer[64];
    M6502_dump(mpu, buffer);
    fprintf(stderr, "6502 state: %s\n", buffer);
}

// Prepare to enter BASIC, returning the address of code which will actually
// enter it.
static uint16_t enter_basic(void) {
    mpu_registers.a = 1; // language entry special value in A
    mpu_registers.x = 0;
    mpu_registers.y = 0;

    const uint16_t code_address = transient_code;
    uint8_t *p = &mpu_memory[code_address];
    *p++ = 0xa2; *p++ = bank_basic;        // LDX #bank_basic
    *p++ = 0x86; *p++ = romsel_copy;       // STX romsel_copy
    *p++ = 0x8e; *p++ = 0x30; *p++ = 0xfe; // STX &FE30
    *p++ = 0x4c; *p++ = 0x00; *p++ = 0x80; // JMP &8000 (language entry)

    return code_address;
}

NORETURN static void callback_abort(const char *type, uint16_t address, uint8_t data) {
    die("error: unexpected %s at address %04x, data %02x", type, address, data);
}

NORETURN static int callback_abort_read(M6502 *mpu, uint16_t address, uint8_t data) {
    // externalise() hasn't been called by lib6502 at this point so we can't
    // dump the registers.
    callback_abort("read", address, data);
}

NORETURN static int callback_abort_write(M6502 *mpu, uint16_t address, uint8_t data) {
    // externalise() hasn't been called by lib6502 at this point so we can't
    // dump the registers.
    callback_abort("write", address, data);
}

NORETURN static int callback_abort_call(M6502 *mpu, uint16_t address, uint8_t data) {
    mpu_dump();
    callback_abort("call", address, data);
}

// Pull an RTS-style return address (i.e. target-1) from the emulated machine's
// stack and return the target address.
static int pull_rts_target(void) {
    uint16_t address = mpu_read_u16(0x101 + mpu_registers.s);
    mpu_registers.s += 2;
    address += 1;
    return address;
}

static int callback_osrdch(M6502 *mpu, uint16_t address, uint8_t data) {
    mpu_state = ms_osrdch_pending;
    longjmp(mpu_env, 1);
}

static int callback_oswrch(M6502 *mpu, uint16_t address, uint8_t data) {
    driver_oswrch(mpu_registers.a);
    return pull_rts_target();
}

static int callback_osnewl(M6502 *mpu, uint16_t address, uint8_t data) {
    driver_oswrch(lf);
    driver_oswrch(cr);
    return pull_rts_target();
}

static int callback_osasci(M6502 *mpu, uint16_t address, uint8_t data) {
    if (mpu_registers.a == cr) {
        return callback_osnewl(mpu, address, data);
    } else {
        return callback_oswrch(mpu, address, data);
    }
}

static int callback_osbyte_return_x(uint8_t x) {
    mpu_registers.x = x;
    return pull_rts_target();
}

static int callback_osbyte_return_u16(uint16_t value) {
    mpu_registers.x = value & 0xff;
    mpu_registers.y = (value >> 8) & 0xff;
    return pull_rts_target();
}

static int callback_osbyte_read_vdu_variable(void) {
    uint8_t i = mpu_registers.x;
    if (vdu_variables[i] == -1) {
        mpu_dump();
        die("internal error: unsupported VDU variable %d read", i);
    }
    uint8_t j = i + 1; // use uint8_t intermediate so we wrap around (unlikely)
    if (vdu_variables[j] == -1) {
        mpu_dump();
        die("internal error: unsupported VDU variable %d read", j);
    }
    mpu_registers.x = vdu_variables[i];
    mpu_registers.y = vdu_variables[j];
    return pull_rts_target();
}

static int callback_osbyte(M6502 *mpu, uint16_t address, uint8_t data) {
    switch (mpu_registers.a) {
        case 0x03: // select output device
            return pull_rts_target(); // treat as no-op
        case 0x0f: // flush buffers
            return pull_rts_target(); // treat as no-op
        case 0x7c: // clear Escape condition
            return pull_rts_target(); // treat as no-op
        case 0x7e: // acknowledge Escape condition
            return callback_osbyte_return_x(0); // no Escape condition pending
        case 0x83: // read OSHWM
            return callback_osbyte_return_u16(page);
        case 0x84: // read HIMEM
            return callback_osbyte_return_u16(himem);
        case 0x86: // read text cursor position
            // We just return with X=Y=0; this is good enough in practice.
            return callback_osbyte_return_u16(0);
        case 0x8a: // place character into buffer
            // ABE uses this to type "OLD<cr>" when re-entering BASIC. It might
            // be nice to emulate this properly, but it also seems silly to
            // complicate the I/O emulation further when we can simply do this
            // explicitly.
            return pull_rts_target(); // treat as no-op
        case 0xa0:
            return callback_osbyte_read_vdu_variable();
        default:
            mpu_dump();
            die("internal error: unsupported OSBYTE");
    }
}

static int callback_oscli(M6502 *mpu, uint16_t address, uint8_t data) {
    uint16_t yx = (mpu_registers.y << 8) | mpu_registers.x;
    // The following case is never going to happen in practice, so let's just
    // explicitly check for it then we don't have to worry about wrapping or
    // accessing past the end of mpu_memory in the following code.
    check(yx <= 0xff00,
          "internal error: command tail is too near top of memory");

    mpu_memory[os_text_pointer    ] = mpu_registers.x;
    mpu_memory[os_text_pointer + 1] = mpu_registers.y;

    // Because our ROMSEL implementation will treat it as an error to page in
    // an empty bank, the following code only works with ABE in banks 0 and 1.
    // This could be changed if necessary.
    assert(bank_editor_a == 0);
    assert(bank_editor_b == 1);
    mpu_registers.a = 4; // unrecognised * command
    mpu_registers.x = bank_editor_b; // first ROM bank to try
    mpu_registers.y = 0; // command tail offset

    // It's tempting to implement a "mini OS" in assembler which would replace
    // the following mixture of C and machine code, as well as other fragments
    // scattered around this file. However, I don't want to create a build
    // dependency on a 6502 assembler and hand-assembling is tedious and
    // error-prone, so this approach minimises the amount of hand-assembled
    // code.

    // Skip leading "*"s on the command; this is essential to have it
    // recognised properly (as that's what the real OS does).
    while (mpu_memory[yx + mpu_registers.y] == '*') {
        ++mpu_registers.y;
        // Y is very unlikely to wrap wround, but be paranoid.
        check(mpu_registers.y != 0, "internal error: too many *s on OSCLI");
    }

    // This isn't case-insensitive and doesn't recognise abbreviations, but
    // in practice it's good enough.
    if (memcmp((char *) &mpu_memory[yx + mpu_registers.y], "BASIC", 5) == 0) {
        return enter_basic();
    }

    const uint16_t code_address = service_code;
    uint8_t *p = &mpu_memory[code_address];
                                           // .loop
    *p++ = 0x86; *p++ = romsel_copy;       // STX romsel_copy
    *p++ = 0x8e; *p++ = 0x30; *p++ = 0xfe; // STX &FE30
    *p++ = 0x20; *p++ = 0x03; *p++ = 0x80; // JSR &8003 (service entry)
    *p++ = 0xa6; *p++ = romsel_copy;       // LDX romsel_copy
    *p++ = 0xca;                           // DEX
    *p++ = 0x10; *p++ = 256 - 13;          // BPL loop
    *p++ = 0xc9; *p++ = 0;                 // CMP #0
    *p++ = 0xd0; *p++ = 1;                 // BNE skip_rts
    *p++ = 0x60;                           // RTS
                                           // .skip_rts
    *p++ = 0x00;                           // BRK
    *p++ = 0xfe;                           // error code
    strcpy((char *) p, "Bad command");     // error string and terminator
    
    return code_address;
}

static int callback_osword_input_line(void) {
    mpu_state = ms_osword_input_line_pending;
    longjmp(mpu_env, 1);
}

static int callback_osword_read_io_memory(void) {
    // We do this access via dynamically generated code so we don't bypass any
    // lib6502 callbacks.
    uint16_t yx = (mpu_registers.y << 8) | mpu_registers.x;
    uint16_t src = mpu_read_u16(yx);
    uint16_t dest = yx + 4;
    const uint16_t code_address = transient_code;
    uint8_t *p = &mpu_memory[code_address];
    *p++ = 0xad; *p++ = src & 0xff; *p++ = (src >> 8) & 0xff;   // LDA src
    *p++ = 0x8d; *p++ = dest & 0xff; *p++ = (dest >> 8) & 0xff; // STA dest
    *p++ = 0x60;                                                // RTS
    return code_address;
}

static int callback_osword(M6502 *mpu, uint16_t address, uint8_t data) {
    switch (mpu_registers.a) {
        case 0x00: // input line
            return callback_osword_input_line();
        case 0x05: // read I/O processor memory
            return callback_osword_read_io_memory();
        default:
            mpu_dump();
            die("internal error: unsupported OSWORD");
    }
}

static int callback_read_escape_flag(M6502 *mpu, uint16_t address,
                                     uint8_t data) {
    return 0; // Escape flag not set
}

static int callback_romsel_write(M6502 *mpu, uint16_t address, uint8_t data) {
    uint8_t *rom_start = &mpu_memory[0x8000];
    switch (data) {
        case bank_editor_a:
            memcpy(rom_start, rom_editor_a, rom_size);
            break;
        case bank_editor_b:
            memcpy(rom_start, rom_editor_b, rom_size);
            break;
        case bank_basic:
            check(config.basic_version != -1,
                  "internal error: no BASIC version selected");
            memcpy(rom_start, rom_basic[config.basic_version], rom_size);
            break;
        default:
            die("internal error: invalid ROM bank %d selected", data);
            break;
    }
    return 0; // return value ignored
}

static int callback_irq(M6502 *mpu, uint16_t address, uint8_t data) {
    // The only possible cause of an interrupt on our emulated machine is a BRK
    // instruction.
    uint16_t error_string_ptr = mpu_read_u16(0x102 + mpu_registers.s);
    mpu_registers.s += 2; // not really necessary, as we're about to exit()
    uint16_t error_num_address = error_string_ptr - 1;
    print_error_prefix();
    fprintf(stderr, "error: ");
    for (uint8_t c; (c = mpu_memory[error_string_ptr]) != '\0';
         ++error_string_ptr) {
        putc(c, stderr);
    }
    uint8_t error_num = mpu_memory[error_num_address];
    fprintf(stderr, " (%d)\n", error_num);
    exit(EXIT_FAILURE);
}

static void callback_poll(M6502 *mpu) {
}

static void set_abort_callback(uint16_t address) {
    M6502_setCallback(mpu, read,  address, callback_abort_read);
    M6502_setCallback(mpu, write, address, callback_abort_write);
}

static void mpu_run() {
    if (setjmp(mpu_env) == 0) {
        mpu_state = ms_running;
        M6502_run(mpu, callback_poll); // returns only via longjmp(mpu_env)
    }
}

void emulation_init(void) {
    mpu = check_alloc(M6502_new(&mpu_registers, mpu_memory, &mpu_callbacks));
    M6502_reset(mpu);
    
    // Install handlers to abort on read or write of anywhere in OS workspace
    // we haven't explicitly allowed; this makes it more obvious if the OS
    // emulation needs to be extended. Addresses 0x90-0xaf are part of OS
    // workspace which we allow access to; they're omitted from the loop to
    // save writing case statements for each of them.
    for (uint16_t address = 0xb0; address < 0x100; ++address) {
        switch (address) {
            case os_text_pointer:
            case os_text_pointer + 1:
            case romsel_copy:
            case 0xfd: // error pointer
            case 0xfe: // error pointer
                // Supported as far as necessary, don't install a handler.
                break;

            default:
                set_abort_callback(address);
                break;
        }
    }

    // Trap access to unimplemented OS vectors.
    for (uint16_t address = 0x200; address < 0x236; ++address) {
        switch (address) {
            case brkv:
            case brkv + 1:
            case wrchv:
            case wrchv + 1:
                // Supported as far as necessary, don't install a handler.
                break;
            default:
                set_abort_callback(address);
                break;
        }
    }

    // Install handlers for OS entry points, using a default for unimplemented
    // ones.
    for (uint16_t address = 0xc000; address != 0; ++address) {
        M6502_setCallback(mpu, call, address, callback_abort_call);
    }
    M6502_setCallback(mpu, call, 0xffe0, callback_osrdch);
    M6502_setCallback(mpu, call, 0xffe3, callback_osasci);
    M6502_setCallback(mpu, call, 0xffe7, callback_osnewl);
    M6502_setCallback(mpu, call, oswrch, callback_oswrch);
    M6502_setCallback(mpu, call, 0xfff1, callback_osword);
    M6502_setCallback(mpu, call, 0xfff4, callback_osbyte);
    M6502_setCallback(mpu, call, 0xfff7, callback_oscli);

    // Install fake OS vectors. Because of the way our implementation works,
    // these vectors actually point to the official entry points.
    mpu_write_u16(wrchv, oswrch);

    // Since we don't have an actual Escape handler, just ensure any read from
    // &ff always returns 0.
    M6502_setCallback(mpu, read, 0xff, callback_read_escape_flag);

    // Install handler for hardware ROM paging emulation.
    M6502_setCallback(mpu, write, 0xfe30, callback_romsel_write);

    // Install interrupt handler so we can catch BRK.
    M6502_setVector(mpu, IRQ, fake_irq_handler);
    M6502_setCallback(mpu, call, fake_irq_handler, callback_irq);

    // Set up VDU variables.
    for (int i = 0; i < 256; ++i) {
        vdu_variables[i] = -1;
    }
    vdu_variables[0x55] = 7; // screen mode
    vdu_variables[0x56] = 4; // memory map type: 1K mode

    mpu_registers.s = 0xff;
    mpu_registers.pc = enter_basic();
    mpu_run();
}

void execute_osrdch(const char *s) {
    // We could in principle handle a multiple character string by returning
    // the values automatically over multiple OSRDCH calls, but we don't need
    // this at the moment.
    assert(s != 0);
    check(strlen(s) == 1,
          "internal error: attempt to return multiple characters from OSRDCH");
    check(mpu_state == ms_osrdch_pending,
          "internal error: emulated machine isn't waiting for OSRDCH");
    mpu_registers.a = s[0];
    mpu_clear_carry(); // no error
    mpu_registers.pc = pull_rts_target();
    mpu_run();
} 

void execute_input_line(const char *line) {
    assert(line != 0);
    check(mpu_state == ms_osword_input_line_pending,
          "internal error: emulated machine isn't waiting for OSWORD 0");
    uint16_t yx = (mpu_registers.y << 8) | mpu_registers.x;
    check(yx <= 0xff00,
          "internal error: OSWORD 0 block is too near top of memory");
    uint16_t buffer = mpu_read_u16(yx);
    check(buffer <= 0xff00,
          "internal error: OSWORD 0 buffer is too near top of memory");
    // mpu_memory[yx + 2] contains the maximum line length; the buffer provided
    // is one byte larger to hold the CR terminator.
    int buffer_size = mpu_memory[yx + 2] + 1;
    size_t pending_length = strlen(line);
    check(pending_length < buffer_size, "error: line too long");
    memcpy(&mpu_memory[buffer], line, pending_length);

    // OSWORD 0 would echo the typed characters and move to a new line, so do
    // the same.
    for (int i = 0; i < pending_length; ++i) {
        driver_oswrch(line[i]);
    }
    driver_oswrch(lf); driver_oswrch(cr);

    mpu_memory[buffer + pending_length] = cr;
    mpu_registers.y = pending_length;
    mpu_clear_carry(); // input not terminated by Escape
    mpu_registers.pc = pull_rts_target();
    mpu_run();
}

// vi: colorcolumn=80
