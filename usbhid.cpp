/*
 * This file is part of senseshock (https://github.com/muhammad23012009/senseshock)
 * Copyright (c) 2025 Muhammad  <thevancedgamer@mentallysanemainliners.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "usbhid.h"
#include "structs.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>
#include <linux/input.h>
#include <filesystem>

// Standard IEEE 802.3 CRC32
static uint32_t crc32_le(uint32_t crc, const uint8_t *p, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return ~crc;
}

// Opens a raw USB interface and exchanges HID packets
UsbHidEmulator::UsbHidEmulator(DualShockEmulator *emulator):
  HidEmulator(emulator)
{
}

UsbHidEmulator::~UsbHidEmulator()
{
    if (m_hidraw_fd >= 0) {
        ::close(m_hidraw_fd);
        m_hidraw_fd = -1;
    }
    if (m_event_fd >= 0) {
        ::close(m_event_fd);
        m_event_fd = -1;
    }
}

void UsbHidEmulator::start(int output_fd)
{
    m_out_fd = output_fd;

    m_hidraw_fd = ::open(m_hidraw_path.c_str(), O_RDWR);
    if (m_hidraw_fd >= 0) {
        std::cout << "Opened hidraw device for HID emulation\n";

        // Setup EVIOCGRAB on corresponding event node
        // hidraw_path is e.g. /dev/hidrawX
        // the sysfs path is /sys/class/hidraw/hidrawX/device/input
        std::string hidraw_name = m_hidraw_path.substr(m_hidraw_path.find_last_of('/') + 1);
        std::string sysfs_input_dir = "/sys/class/hidraw/" + hidraw_name + "/device/input";

        // Find the event node inside the input directory safely using filesystem API
        std::string event_node = "";
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(sysfs_input_dir)) {
                if (entry.is_directory()) {
                    std::string dir_name = entry.path().filename().string();
                    if (dir_name.find("event") == 0) { // Check if dir_name starts with "event"
                        event_node = "/dev/input/" + dir_name;
                        break;
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error while finding event node: " << e.what() << "\n";
        }

        if (!event_node.empty()) {
            int event_fd = ::open(event_node.c_str(), O_RDWR);
            if (event_fd >= 0) {
                if (ioctl(event_fd, EVIOCGRAB, 1) == 0) {
                    std::cout << "Successfully grabbed event node " << event_node << " with EVIOCGRAB\n";
                    // Need to keep event_fd open to maintain grab
                    m_event_fd = event_fd;
                } else {
                    std::cerr << "Failed to EVIOCGRAB on " << event_node << "\n";
                    ::close(event_fd);
                }
            } else {
                std::cerr << "Failed to open event node " << event_node << "\n";
            }
        } else {
            std::cerr << "Could not find corresponding event node for " << m_hidraw_path << "\n";
        }

    } else {
        std::cerr << "Failed to open hidraw device\n";
    }
}

void UsbHidEmulator::hid_get_feature(uint8_t report_number, uint8_t interface_number, uint8_t len, uint8_t *out)
{
    // Unused in BT mode but kept to satisfy interface
}

void UsbHidEmulator::hid_set_feature(uint8_t report_number, uint8_t interface_number, uint8_t len, uint8_t *in)
{
    // Unused in BT mode but kept to satisfy interface
}

void UsbHidEmulator::hid_send_report(dualsense_output_report_common report)
{
    // Unused in BT mode but kept to satisfy interface
}

void UsbHidEmulator::hid_send_report_bt(uint8_t* buf)
{
    if (m_hidraw_fd < 0) return;

    uint8_t bt_out[78];
    memset(bt_out, 0, sizeof(bt_out));

    bt_out[0] = 0x11;
    bt_out[1] = 0xc0;
    bt_out[2] = 0x20;
    memcpy(bt_out + 3, buf + 1, 31); // skip report id 0x05

    uint8_t crc_buf[75];
    crc_buf[0] = 0xA2;
    memcpy(crc_buf + 1, bt_out, 74);

    uint32_t crc = crc32_le(0, crc_buf, 75);

    bt_out[74] = crc & 0xFF;
    bt_out[75] = (crc >> 8) & 0xFF;
    bt_out[76] = (crc >> 16) & 0xFF;
    bt_out[77] = (crc >> 24) & 0xFF;

    ::write(m_hidraw_fd, bt_out, sizeof(bt_out));
}

// Starts an asynchronous search for a compatible USB device
void UsbHidEmulator::search_for_device()
{
    // Bypassing hotplug logic since hidraw_path is provided via CLI
    if (!m_hidraw_path.empty()) {
        get_emulator()->setup_ep0();
        get_emulator()->setup_dualsense(this);
    }
}

void UsbHidEmulator::stop_device_search()
{
}

static uint8_t s_buffer[128];

void UsbHidEmulator::process_device_events()
{
    if (m_hidraw_fd < 0)
        return;

    int ret = ::read(m_hidraw_fd, s_buffer, sizeof(s_buffer));

    if (ret >= 78 && s_buffer[0] == 0x11) {
        uint8_t uhid_buf[64];
        uhid_buf[0] = 0x01;
        memcpy(uhid_buf + 1, s_buffer + 3, 63);

        get_emulator()->uhid_send_input(uhid_buf, sizeof(uhid_buf));
    }
}
