/*
 * hle_sega.c - the cabinet's own library, answered from here.
 *
 * A Lindbergh game does not talk to its hardware directly. It calls SEGA's
 * amLib: a C API, statically linked into this binary, that reaches a base
 * board over a serial link - the JVS I/O board with the coin slots, buttons
 * and guns on it, the EEPROM holding the bookkeeping, the security dongle.
 * None of that exists here, so every one of those calls fails and the game
 * stops on "Error 11 - JVS I/O board is not connected to main board" before
 * it ever reaches its attract mode.
 *
 * These are plain C symbols, so they override by name the same way the
 * XF86VidMode entry points do. What is answered here is only what the game
 * actually asks on the way to attract mode; the game narrates the rest
 * through _sDebug::putConsole (LINDBERGH_CONSOLE=1), which is how each of
 * these was found rather than guessed.
 */

#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"

/* The library's own success code. Every am* function returns 0 for success
 * and a negative value for a failure the game then reports. */
#define AM_OK 0

/* The JVS packet the game handed to the transport.
 *
 * Request layout, read off amJvspClearPacket and amJvsSendRequest: byte 0 is
 * the node the packet is addressed to, byte 1 the length of the frame, and
 * bytes 2.. the frame itself. The acknowledge comes back in the same object
 * at byte 0x101 (length) and 0x102.. (frame).
 *
 * LINDBERGH_JVS=1 prints what is asked, which is the only honest way to find
 * out what this board has to be able to answer. */
static void jvs_dump(const char *tag, uint32_t pkt, unsigned off, unsigned len)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)pkt;
    char line[512];
    int at = snprintf(line, sizeof line, "[jvs] %s node %u len %u:", tag, p[0], len);
    for (unsigned i = 0; i < len && i < 40 && at < 470; i++)
        at += snprintf(line + at, sizeof line - at, " %02X", p[off + i]);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* ---- the I/O board itself ----
 *
 * JVS is a small request/response protocol over a serial line. A frame is
 *
 *     E0 <dest> <count> <payload...> <sum>
 *
 * where count covers everything after itself including the checksum, and the
 * checksum is the bytes from <dest> to the last payload byte, added up and
 * truncated to eight bits. A reply is the same shape addressed to 00, with a
 * status byte ahead of the reports and one report per command asked.
 *
 * The packet object carries more than the frame. Bytes 1.. of a REQUEST hold
 * a zero-terminated list of where each command starts inside it, built as the
 * game appended them; the matching ACK has to come back with the same list
 * pointing at where each report starts. That list is why this does not need
 * to know how long any command is - the game already said.
 *
 * Nothing here is pressed, inserted or aimed. An attract mode wants a board
 * that answers, not one that plays.
 */

#define JVS_SYNC      0xE0u
#define JVS_STATUS_OK 0x01u
#define JVS_REPORT_OK 0x01u

#define PKT_INDEX     1u        /* command / report offsets, zero-terminated */
#define PKT_FRAME_LEN 0x101u
#define PKT_FRAME     0x102u

/* What the board calls itself. The game reads this back as a C string and
 * shows it in the test menu; the shape is the one every SEGA I/O board of
 * this era reports. */
static const char JVS_IO_IDENT[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Switches, coins, analog channels, and general purpose output - enough for a
 * two player gun cabinet. Each block is kind, then three parameters. */
static const unsigned char JVS_FEATURES[] = {
    0x01, 2, 12, 0,        /* players, buttons each */
    0x02, 2,  0, 0,        /* coin slots */
    0x03, 8, 16, 0,        /* analog channels, bits each */
    0x12, 6,  0, 0,        /* general purpose output */
    0x00                   /* end of list */
};

/* One reply at a time: the game sends, then receives, on the same thread. */
static unsigned char g_ack[0x200];
static unsigned      g_ack_len;
static unsigned char g_report_at[0x40];
static unsigned      g_reports;

static void ack_begin(void)
{
    g_ack_len = 0; g_reports = 0;
    g_ack[g_ack_len++] = JVS_SYNC;
    g_ack[g_ack_len++] = 0x00;              /* addressed to the main board */
    g_ack[g_ack_len++] = 0x00;              /* count, filled in at the end */
    g_ack[g_ack_len++] = JVS_STATUS_OK;
}

/* Remember where this report starts before writing it: that offset is what
 * the game looks up to find the answer to the command it asked. */
static void ack_report(void)
{
    if (g_reports < sizeof g_report_at) g_report_at[g_reports++] = (unsigned char)g_ack_len;
    g_ack[g_ack_len++] = JVS_REPORT_OK;
}

static void ack_byte(unsigned char b)
{
    if (g_ack_len < sizeof g_ack - 1) g_ack[g_ack_len++] = b;
}

static void ack_zeros(unsigned n) { while (n--) ack_byte(0); }

static void ack_end(void)
{
    unsigned sum = 0;
    g_ack[2] = (unsigned char)(g_ack_len - 3 + 1);   /* payload + checksum */
    for (unsigned i = 1; i < g_ack_len; i++) sum += g_ack[i];
    g_ack[g_ack_len++] = (unsigned char)sum;
}

/* Answer one command. `a` is the command byte, `p` its parameters. */
static void jvs_command(const unsigned char *p)
{
    switch (p[0]) {
    case 0x10:                                   /* read board identity */
        ack_report();
        for (const char *s = JVS_IO_IDENT; *s; s++) ack_byte((unsigned char)*s);
        ack_byte(0);
        break;
    case 0x11: ack_report(); ack_byte(0x13); break;   /* command revision */
    case 0x12: ack_report(); ack_byte(0x30); break;   /* JVS revision */
    case 0x13: ack_report(); ack_byte(0x10); break;   /* communication version */
    case 0x14:                                   /* what this board has */
        ack_report();
        for (unsigned i = 0; i < sizeof JVS_FEATURES; i++) ack_byte(JVS_FEATURES[i]);
        break;
    case 0x20: {                                 /* switches: test, then players */
        /* One test byte, then `bytes` bytes for each of `players`. In the
         * first of those, bit 7 is start and bit 1 is the first push button,
         * which for a gun cabinet is the trigger. */
        CabinetInput in;
        host_cabinet_input(&in);
        unsigned players = p[1], bytes = p[2];
        {   /* Confirm the game is actually asking, and with what shape. */
            static int shown;
            if (shown < 3 && getenv("LINDBERGH_JVS")) {
                shown++;
                fprintf(stderr, "[jvs] switch read: %u players x %u bytes\n",
                        players, bytes);
            }
        }
        ack_report();
        ack_byte((in.buttons & CAB_TEST) ? 0x80 : 0x00);
        for (unsigned pl = 0; pl < players; pl++) {
            unsigned char b0 = 0;
            if (pl == 0) {
                if (in.buttons & CAB_P1_START)   b0 |= 0x80;
                if (in.buttons & CAB_SERVICE)    b0 |= 0x40;
                if (in.buttons & CAB_P1_TRIGGER) b0 |= 0x02;
                if (in.buttons & CAB_P1_RELOAD)  b0 |= 0x01;
            } else if (pl == 1) {
                if (in.buttons & CAB_P2_START)   b0 |= 0x80;
            }
            ack_byte(b0);
            ack_zeros(bytes > 1 ? bytes - 1 : 0);
        }
        break;
    }
    case 0x21: {                                 /* coins */
        /* Two bytes a slot: the top two bits are the mechanism's condition,
         * zero meaning normal, and the remaining fourteen are the count. */
        CabinetInput in;
        host_cabinet_input(&in);
        ack_report();
        for (unsigned slot = 0; slot < p[1]; slot++) {
            unsigned n = (slot < 2 ? in.coins[slot] : 0) & 0x3FFF;
            ack_byte((unsigned char)(n >> 8));
            ack_byte((unsigned char)(n & 0xFF));
        }
        break;
    }
    case 0x22: {                                 /* analog: where the gun points */
        CabinetInput in;
        host_cabinet_input(&in);
        int w = in.screen_w > 0 ? in.screen_w : 1;
        int h = in.screen_h > 0 ? in.screen_h : 1;
        unsigned x = (unsigned)((long)in.gun_x * 65535 / w);
        unsigned y = (unsigned)((long)in.gun_y * 65535 / h);
        if (in.gun_x < 0) x = 0; if (x > 65535) x = 65535;
        if (in.gun_y < 0) y = 0; if (y > 65535) y = 65535;
        ack_report();
        {   /* LINDBERGH_NO_GUN=1 reports a centred, still gun. A test, not a
             * setting: it tells apart a game upset by where the gun is from a
             * game upset by something else entirely. */
            static int flat = -1;
            if (flat < 0) { const char *v = getenv("LINDBERGH_NO_GUN");
                            flat = (v && *v && *v != '0') ? 1 : 0; }
            for (unsigned ch = 0; ch < p[1]; ch++) {
                unsigned v = flat ? 0x8000u
                           : (ch == 0 || ch == 2) ? x
                           : (ch == 1 || ch == 3) ? y : 0x8000u;
                ack_byte((unsigned char)(v >> 8));
                ack_byte((unsigned char)(v & 0xFF));
            }
        }
        break;
    }
    case 0x23: ack_report(); ack_zeros((unsigned)p[1] * 2u); break;  /* rotary */
    default:                                     /* acknowledged, nothing to say */
        ack_report();
        break;
    }
}

static void h_jvs_send(CPU *c)
{
    uint32_t pkt = A32(0);
    if (!pkt) { RET(-7); return; }
    const unsigned char *p = (const unsigned char *)(uintptr_t)pkt;
    const unsigned char *frame = p + 2;

    if (getenv("LINDBERGH_JVS")) jvs_dump("req", pkt, 2, p[1]);

    ack_begin();
    for (unsigned i = 0; i < 0x40 && p[PKT_INDEX + 0x101 + i]; i++) {
        /* The command index lives at 0x102 in the request, where the reply
         * frame lives in an acknowledge packet. Same offset, different use. */
        unsigned off = p[PKT_FRAME + i];
        /* The guard is against running off the packet, not off the frame: a
         * command sits at the end of its frame and its parameters are read
         * past p[1] quite legitimately. Comparing against the frame length
         * rejected every single-command request there is. */
        if (off < 3u || off + 2u >= 0x100u) break;
        jvs_command(frame + off);
    }
    ack_end();
    RET(AM_OK);
}

static void h_jvs_recv(CPU *c)
{
    uint32_t ack = A32(1);
    if (!ack || !g_ack_len) { RET(-6); return; }
    unsigned char *a = (unsigned char *)(uintptr_t)ack;

    memcpy(a + PKT_FRAME, g_ack, g_ack_len);
    a[PKT_FRAME_LEN] = (unsigned char)g_ack_len;
    /* The report offsets, in the order the commands were asked, zero
     * terminated - the same shape amJvspMakeReportIndex would have left. */
    for (unsigned i = 0; i < g_reports; i++) a[PKT_INDEX + i] = g_report_at[i];
    a[PKT_INDEX + g_reports] = 0;

    if (getenv("LINDBERGH_JVS")) jvs_dump("ack", ack, PKT_FRAME, g_ack_len);
    RET(AM_OK);
}

static void h_ok(CPU *c)   { RET(AM_OK); }
static void h_true(CPU *c) { RET(1); }
static void h_one(CPU *c)  { RET(1); }

/* A base board serial, in the shape the cabinet's own would take. */
static void h_serial_id(CPU *c)
{
    static const char id[] = "AAFE-01A00000000";
    uint32_t out = A32(0);
    if (out) memcpy((void *)(uintptr_t)out, id, sizeof id);
    RET(AM_OK);
}

/* ---- the cabinet's non-volatile memory ----
 *
 * Bookkeeping, settings and credits live in two places on a Lindbergh: a
 * battery-backed SRAM on the base board and a small EEPROM. The game reaches
 * both through four wrapper functions, and everything above them - record
 * layout, duplicate copies, CRCs, validation - is the game's own code, which
 * works perfectly well as long as the bytes come back.
 *
 * So this is the whole of it: two byte arrays that persist to a file. Nothing
 * models a record, because nothing needs to.
 */

#define SRAM_SIZE   0x40000u
#define EEPROM_SIZE 0x10000u

static unsigned char g_sram[SRAM_SIZE];
static unsigned char g_eeprom[EEPROM_SIZE];
static int           g_nvram_loaded;

static const char *nvram_path(void)
{
    static char path[512];
    if (!*path) {
        const char *p = getenv("LINDBERGH_NVRAM");
        snprintf(path, sizeof path, "%s", (p && *p) ? p : "lindbergh_nvram.bin");
    }
    return path;
}

static void nvram_load(void)
{
    if (g_nvram_loaded) return;
    g_nvram_loaded = 1;
    FILE *f = fopen(nvram_path(), "rb");
    if (!f) return;
    if (fread(g_sram, 1, sizeof g_sram, f) != sizeof g_sram ||
        fread(g_eeprom, 1, sizeof g_eeprom, f) != sizeof g_eeprom) {
        /* A short or corrupt file is not worth keeping: the game rebuilds its
         * records from defaults the moment it finds them invalid. */
        memset(g_sram, 0, sizeof g_sram);
        memset(g_eeprom, 0, sizeof g_eeprom);
    }
    fclose(f);
}

static void nvram_save(void)
{
    FILE *f = fopen(nvram_path(), "wb");
    if (!f) return;
    fwrite(g_sram, 1, sizeof g_sram, f);
    fwrite(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
}

/* The wrappers answer 0, or -12 the way the real ones do when the base board
 * does not reply. An access outside the store is that failure. */
#define AM_BACKUP_FAIL (-12)

static int nvram_copy(unsigned char *store, unsigned size, uint32_t guest,
                      uint32_t addr, uint32_t len, int writing)
{
    nvram_load();
    if (!guest || addr > size || len > size - addr) return AM_BACKUP_FAIL;
    void *g = (void *)(uintptr_t)guest;
    if (writing) { memcpy(store + addr, g, len); nvram_save(); }
    else           memcpy(g, store + addr, len);
    return AM_OK;
}

/* Each wrapper reorders its arguments on the way down to a different device,
 * so which of the three is the buffer is not the same for any two of them.
 * LINDBERGH_NVLOG=1 prints all three and lets their magnitude say: a guest
 * pointer is up in the image, an offset is small. Read takes the offset
 * first, write takes the buffer first - reading both the same way puts a
 * pointer's value in as an address and reports a broken record. */
static void nv_log(const char *who, CPU *c)
{
    if (getenv("LINDBERGH_NVLOG"))
        fprintf(stderr, "[nv] %-10s a0=0x%08X a1=0x%08X a2=0x%08X\n",
                who, A32(0), A32(1), A32(2));
}

static void h_sram_read(CPU *c)
{ nv_log("sramRead", c);  RET(nvram_copy(g_sram, SRAM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_sram_write(CPU *c)
{ nv_log("sramWrite", c); RET(nvram_copy(g_sram, SRAM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* The EEPROM pair reorders differently again and has not been reached yet;
 * these follow the SRAM shape and the log will say if that is wrong. */
static void h_eeprom_read(CPU *c)
{ nv_log("eepRead", c);   RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_eeprom_write(CPU *c)
{ nv_log("eepWrite", c);  RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* ---- the error the blank store raises ----
 *
 * acpSystem keeps one error code in a global and paints it over the screen
 * once set; anything other than -1 or -2 comes out as "Error 15 - Game
 * Program Not Found". The backup layer sets -3 when its records fail their
 * CRC, which they do here because the store is blank: the game's own repair
 * path writes through an EEPROM that lives on an I2C bus this runtime does
 * not have, so the records can never become valid and the error is permanent.
 *
 * A cabinet ships with that store already written. Suppressing this one code
 * is the same statement - the bookkeeping is assumed good - and it is scoped
 * to exactly that: every other error still reaches the game untouched, and
 * the suppression announces itself once on the way past.
 *
 * setError is reimplemented rather than skipped, which means knowing the two
 * globals it touches. Those are read out of the function's own first bytes
 * instead of being written down here, so this survives a different build of
 * the same framework:
 *
 *     55                 push ebp
 *     A1 <error var>     mov  eax, [error var]
 *     80 3D <latch> 00   cmp  byte [latch], 0
 *     ...
 *     A3/89 05 <var>     mov  [error var], eax
 */
#define ACP_ERR_BACKUP (-3)

static uint32_t g_err_var, g_err_latch;

static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_error_globals(uint32_t va)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)va;
    for (unsigned i = 0; i < 24; i++) {
        if (!g_err_var && p[i] == 0xA1) g_err_var = rd32le(p + i + 1);
        if (!g_err_latch && p[i] == 0x80 && p[i + 1] == 0x3D)
            g_err_latch = rd32le(p + i + 2);
    }
    return g_err_var != 0;
}

static void h_set_error(CPU *c)
{
    int32_t v = (int32_t)A32(0);

    if (v == ACP_ERR_BACKUP) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[sega] backup records are blank and cannot be "
                            "initialised without the EEPROM bus; treating the "
                            "bookkeeping as good\n");
        }
        RET(0);
        return;
    }

    /* Faithful otherwise: the original only stores while the latch is clear,
     * so that the first error to happen is the one that gets reported. */
    if (g_err_var &&
        (!g_err_latch || *(const unsigned char *)(uintptr_t)g_err_latch == 0))
        *(uint32_t *)(uintptr_t)g_err_var = (uint32_t)v;

    if (getenv("LINDBERGH_ACPERR")) {
        fprintf(stderr, "[acp] setError(%d) from:\n", (int)v);
        guest_backtrace(c);
    }
    RET(0);
}

/* ---- the security dongle ----
 *
 * A Lindbergh cabinet carries a hardware dongle, and a game does not simply
 * ask whether it is present. Ghost Squad Evolution generates sixteen random
 * bytes, has the dongle transform them, and checks the answer. The key for
 * that transform lives inside the device; it is not in the disc, not in the
 * binary, and not recoverable from either.
 *
 * So there is nothing to emulate here and no cleverness that would help. The
 * cabinet this game ran on had the dongle fitted, and the runtime makes the
 * same statement - the licence is present - by answering the one predicate
 * the game gates on, and saying so once on the way past. The challenge and
 * response themselves are left entirely alone.
 *
 * Bound by symbol name, so it applies to a game that has these and is simply
 * absent from one that does not. */
static void h_dongle_valid(CPU *c)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[sega] no security dongle is fitted; treating the "
                        "licence as present\n");
    }
    RET(1);
}

void hle_register_sega(void)
{
    int n = 0;

    /* Ghost Squad Evolution gates its whole boot on this one predicate;
     * a game without these symbols simply does not get the override. */
    n += guest_override("_Z18is_dongle_validatev", h_dongle_valid);
    n += guest_override("_Z21check_dongle_validatev", h_dongle_valid);

    /* The base board. amLibInit failing is what produces "SEGA BaseBD not
     * available", and everything else is gated behind it. */
    n += guest_override("amLibInit", h_ok);
    n += guest_override("amLibExit", h_ok);
    n += guest_override("amLibIsBasebdAvailable", h_true);

    /* The JVS link itself. amJvsCheckInit is a predicate, not a status code:
     * amJvsGetNodes reads it as a boolean and answers -5 when it is false,
     * which is how answering it with the library's success value of 0 turned
     * into "-5 JVS node(s) found". */
    n += guest_override("amJvsInit", h_ok);
    n += guest_override("amJvsExit", h_ok);
    n += guest_override("amJvsCheckInit", h_true);

    /* One I/O board on the link - the cabinet's own, with the buttons, the
     * coin slots and the guns. */
    n += guest_override("amJvsGetNodes", h_one);

    /* The security dongle. Nothing here reads a real one, and the game only
     * asks whether it is present before carrying on. */
    n += guest_override("amDongleInit", h_ok);
    n += guest_override("amDongleExit", h_ok);
    n += guest_override("amDongleIsAvailable", h_true);

    /* The dipswitches and the bookkeeping the game reads at start up. It
     * asks for these by name in its console when they are missing. */
    n += guest_override("amDipswInit", h_ok);
    n += guest_override("amDipswExit", h_ok);

    /* acpSystem::checkDongle polls amDongleUpdate and raises "Error 15 -
     * Game Program Not Found" the moment it answers anything but zero. */
    n += guest_override("amDongleUpdate", h_ok);

    /* The base board's serial. The game prints it and stores it with its
     * bookkeeping; nothing here depends on the value, only on having one. */
    n += guest_override("amOsinfoGetBaseBoardSerialId", h_serial_id);

    /* The battery-backed store the bookkeeping lives in. */
    n += guest_override("amEepromInit", h_ok);
    n += guest_override("amEepromExit", h_ok);
    n += guest_override("amBackupWrapper_BbBuSramRead", h_sram_read);
    n += guest_override("amBackupWrapper_BbBuSramWrite", h_sram_write);
    n += guest_override("amBackupWrapper_KeyEepromRead", h_eeprom_read);
    n += guest_override("amBackupWrapper_KeyEepromWrite", h_eeprom_write);

    /* LINDBERGH_NO_JVS=1 leaves the board unanswered, so the game finds no
     * I/O and says so. A test for whether a fault lives in this transport or
     * merely happens to be near it. */
    if (!getenv("LINDBERGH_NO_JVS")) {
        n += guest_override("amJvsSendRequest", h_jvs_send);
        n += guest_override("amJvsRecvAcknowledge", h_jvs_recv);
    }

    {   /* Only worth installing if the two globals can be found; without
         * them this would swallow every error instead of one. */
        uint32_t va = guest_symbol("_ZN9acpSystem8setErrorEl");
        if (va && find_error_globals(va))
            n += guest_override("_ZN9acpSystem8setErrorEl", h_set_error);
    }

    fprintf(stderr, "[sega] %d base board entry points answered\n", n);
}
