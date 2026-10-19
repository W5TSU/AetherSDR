#!/usr/bin/env python3
"""Fail if src/ uses a libhackrf name the oldest supported libhackrf lacks.

The x86_64 AppImage builds on Ubuntu 22.04, whose libhackrf-dev is 2021.03.1.
Newer headers add names (BOARD_ID_UNDETECTED, BOARD_ID_PRALINE, ...) that
compile everywhere else, so the break only shows when a release tag builds
the AppImage (v26.10.6 shipped without one; #67 was the same class). This
scans src/ for libhackrf-shaped identifiers outside comments and strings and
checks each against the names that header declares.

Raising the floor (the AppImage moves to a newer Ubuntu): regenerate
DECLARED_2021_03_1 from that release's host/libhackrf/src/hackrf.h.

Usage: python3 tools/check_libhackrf_api.py [--root DIR]
"""

import argparse
import pathlib
import re
import sys

# Every libhackrf identifier declared by hackrf.h at tag v2021.03.1.
DECLARED_2021_03_1 = frozenset("""
BOARD_ID_HACKRF_ONE BOARD_ID_INVALID BOARD_ID_JAWBREAKER BOARD_ID_JELLYBEAN
BOARD_ID_RAD1O BYTES_PER_BLOCK HACKRF_ERROR_BUSY HACKRF_ERROR_INVALID_PARAM
HACKRF_ERROR_LIBUSB HACKRF_ERROR_NOT_FOUND HACKRF_ERROR_NOT_LAST_DEVICE
HACKRF_ERROR_NO_MEM HACKRF_ERROR_OTHER HACKRF_ERROR_STREAMING_EXIT_CALLED
HACKRF_ERROR_STREAMING_STOPPED HACKRF_ERROR_STREAMING_THREAD_ERR
HACKRF_ERROR_THREAD HACKRF_ERROR_USB_API_VERSION HACKRF_ISSUE_609_IS_FIXED
HACKRF_SUCCESS HACKRF_TRUE MAX_SWEEP_RANGES RF_PATH_FILTER_BYPASS
RF_PATH_FILTER_HIGH_PASS RF_PATH_FILTER_LOW_PASS SAMPLES_PER_BLOCK
USB_BOARD_ID_HACKRF_ONE USB_BOARD_ID_INVALID USB_BOARD_ID_JAWBREAKER
USB_BOARD_ID_RAD1O hackrf_board_id hackrf_board_id_name hackrf_board_id_read
hackrf_board_partid_serialno_read hackrf_close hackrf_compute_baseband_filter_bw
hackrf_compute_baseband_filter_bw_round_down_lt hackrf_cpld_checksum
hackrf_cpld_write hackrf_device hackrf_device_list hackrf_device_list_free
hackrf_device_list_open hackrf_device_list_t hackrf_error hackrf_error_name
hackrf_exit hackrf_filter_path_name hackrf_get_operacake_boards hackrf_init
hackrf_init_sweep hackrf_is_streaming hackrf_library_release
hackrf_library_version hackrf_max2837_read hackrf_max2837_write hackrf_open
hackrf_open_by_serial hackrf_operacake_gpio_test hackrf_reset
hackrf_rffc5071_read hackrf_rffc5071_write hackrf_sample_block_cb_fn
hackrf_set_amp_enable hackrf_set_antenna_enable
hackrf_set_baseband_filter_bandwidth hackrf_set_clkout_enable hackrf_set_freq
hackrf_set_freq_explicit hackrf_set_hw_sync_mode hackrf_set_lna_gain
hackrf_set_operacake_ports hackrf_set_operacake_ranges hackrf_set_sample_rate
hackrf_set_sample_rate_manual hackrf_set_txvga_gain hackrf_set_ui_enable
hackrf_set_vga_gain hackrf_si5351c_read hackrf_si5351c_write
hackrf_spiflash_clear_status hackrf_spiflash_erase hackrf_spiflash_read
hackrf_spiflash_status hackrf_spiflash_write hackrf_start_rx
hackrf_start_rx_sweep hackrf_start_tx hackrf_stop_rx hackrf_stop_tx
hackrf_transfer hackrf_usb_api_version_read hackrf_usb_board_id
hackrf_usb_board_id_name hackrf_version_string_read
""".split())

# The prefixes libhackrf's names use. \b keeps AETHER_BACKEND_HACKRF and
# kBoardIdPraline out: neither starts a word with one of these.
LIBHACKRF_NAME = re.compile(
    r"\b(?:hackrf_\w+|HACKRF_\w+|(?:USB_)?BOARD_ID_\w+|BOARD_REV_\w+"
    r"|RF_PATH_FILTER_\w+|TRANSCEIVER_MODE_\w+|SWEEP_STYLE_\w+)\b")

# Comments, then string and character literals; whichever starts first wins,
# so a // inside a string and a quote inside a comment are both handled.
NOT_CODE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}


def code_only(text):
    """The text with comments and literals blanked, line numbers kept."""
    return NOT_CODE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def undeclared_uses(text):
    """(line, name) for each libhackrf-shaped name the floor does not declare."""
    found = []
    code = code_only(text)
    for m in LIBHACKRF_NAME.finditer(code):
        name = m.group(0)
        if name not in DECLARED_2021_03_1:
            found.append((code.count("\n", 0, m.start()) + 1, name))
    return found


def scan(root):
    problems = []
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "hackrf" not in text.lower():
            continue
        for line, name in undeclared_uses(text):
            problems.append(f"{path.relative_to(root)}:{line}: {name}")
    return problems


def self_test():
    """The scanner's own edge cases, so a broken regex cannot pass silently."""
    sample = (
        "// BOARD_ID_UNDETECTED in a comment\n"
        "const char* s = \"BOARD_ID_PRALINE\";\n"
        "/* hackrf_tx_modes_test */ int AETHER_BACKEND_HACKRF = 0;\n"
        "uint8_t id = BOARD_ID_INVALID;\n"
        "uint8_t bad = BOARD_ID_UNDETECTED;\n")
    got = undeclared_uses(sample)
    assert got == [(5, "BOARD_ID_UNDETECTED")], got


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    self_test()
    problems = scan(args.root)
    if problems:
        print("libhackrf names missing from 2021.03.1, the x86_64 AppImage's "
              "Ubuntu 22.04 package. Use the number (see HackRfWorker::isPro) "
              "or guard the call:")
        for p in problems:
            print("  " + p)
        return 1
    print("libhackrf API floor: every name src/ uses is in 2021.03.1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
