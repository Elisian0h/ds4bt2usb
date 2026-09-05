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

#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/mount.h>
#include <linux/types.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <linux/hid.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>

#include "senseshock.h"
#include "usbhid.h"
#include "structs.h"

static constexpr char ds4_firmware_build_date[] = "Mar 25 2016";
static constexpr char ds4_firmware_build_time[] = "12:00:00";

struct functionfs_descriptor {
    usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;

    struct {
        usb_interface_descriptor intf;
        hid_descriptor hid;
        usb_endpoint_descriptor_no_audio ep_in;
        usb_endpoint_descriptor_no_audio ep_out;
    } __attribute__((packed)) descs[2];
} __attribute__((packed));

#define STR_INTERFACE_ "Source/Sink"

static const struct {
        struct usb_functionfs_strings_head header;
        struct {
                __le16 code;
                const char str1[sizeof STR_INTERFACE_];
        } __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
        .header = {
                .magic = FUNCTIONFS_STRINGS_MAGIC,
                .length = (sizeof strings),
                .str_count = (1),
                .lang_count = (1),
        },
        .lang0 = {
                __cpu_to_le16(0x0409), /* en-us */
                STR_INTERFACE_,
        },
};

// Descriptors copied from an actual DualShock 4
unsigned char descs[] = {
    #include "descriptors.inc"
};

DualShockEmulator::~DualShockEmulator()
{
    m_control_thread_running = false;
    m_io_thread_running = false;
    if (m_control_thread.joinable())
        m_control_thread.join();
    if (m_io_thread.joinable())
        m_io_thread.join();
    if (ep0_fd >= 0)
        ::close(ep0_fd);
    if (ep1_in_fd >= 0)
        ::close(ep1_in_fd);
    if (ep2_out_fd >= 0)
        ::close(ep2_out_fd);

    if (m_functionfs_setup) {
        struct uhid_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = UHID_DESTROY;
        ::write(ep0_fd, &ev, sizeof(ev));
    }

    delete m_hid;
}

void DualShockEmulator::setup_dualsense(HidEmulator *emulator)
{
    // Start the HID emulator
    m_hid = emulator;
    dualsense_setup = true;
}

void DualShockEmulator::dualsense_disconnected()
{
    dualsense_setup = false;
}

int DualShockEmulator::handle_get_report(uint8_t report_id, uint8_t *buffer)
{
    // Handle the reportID's and send appropriate responses

    std::cout << "Handling get_report for report ID: " << std::hex << (int)report_id << std::dec << "\n";

    switch (report_id) {
    case DS4_FEATURE_GYRO_CALIBRATION: {
        std::cout << "Forwarding gyro calibration request to DualSense\n";
        // Time to delegate this to the DualSense.
        m_hid->hid_get_feature(DS_FEATURE_GYRO_CALIBRATION, 3, DS_FEATURE_GYRO_CALIBRATION_LEN, buffer);        
        buffer[0] = DS4_FEATURE_GYRO_CALIBRATION; // Restore report ID
        return DS4_FEATURE_GYRO_CALIBRATION_LEN;
    }

    case DS4_FEATURE_PAIRING_INFO:
        // Copy the MAC address from the DualSense.
        std::cout << "Copying pairing info from the DualSense\n";
        m_hid->hid_get_feature(DS_FEATURE_PAIRING_INFO, 3, DS_FEATURE_PAIRING_INFO_LEN, buffer);
        buffer[0] = DS4_FEATURE_PAIRING_INFO;
        return DS4_FEATURE_PAIRING_INFO_LEN;

    case DS4_FEATURE_HW_FW_VERSION: {
        dualshock_feature_report_firmware ds4_firmware;
        std::memset(&ds4_firmware, 0, sizeof(ds4_firmware));

        ds4_firmware.report_id = DS4_FEATURE_HW_FW_VERSION;

        std::memcpy(ds4_firmware.build_date, ds4_firmware_build_date, sizeof(ds4_firmware_build_date));
        std::memcpy(ds4_firmware.build_time, ds4_firmware_build_time, sizeof(ds4_firmware_build_time));

        // Needed by some games, e.g. Detroit: Become Human sanity-checks hw_version
        ds4_firmware.hw_version_major = 0x0100;
        ds4_firmware.hw_version_minor = 0x6414;
        ds4_firmware.fw_version_major = 0x00000001;
        ds4_firmware.fw_version_minor = 0x7007;

        std::memcpy(buffer, &ds4_firmware, sizeof(ds4_firmware));

        return DS4_FEATURE_HW_FW_VERSION_LEN;
    }
    }

    return 1;
}

void DualShockEmulator::uhid_send_input(uint8_t* buf, size_t size)
{
    if (ep0_fd < 0) return;

    struct uhid_event in_ev;
    memset(&in_ev, 0, sizeof(in_ev));
    in_ev.type = UHID_INPUT2;
    in_ev.u.input2.size = size;
    memcpy(in_ev.u.input2.data, buf, size);

    ::write(ep0_fd, &in_ev, sizeof(in_ev));
}

int DualShockEmulator::handle_set_report(uint8_t *buffer, size_t length)
{
    dualshock4_output_report *in_report = (dualshock4_output_report *)buffer;
    dualsense_output_report_common out_report;
    std::memset(&out_report, 0, sizeof(dualsense_output_report_common));

    /* Map the flags first and foremost. */
    if (in_report->valid_flag0 & 0x01) { // Motor update
        out_report.valid_flag0 |= (1 << 0) | (1 << 1); // Vibration v0
        out_report.motor_right = in_report->motor_right;
        out_report.motor_left = in_report->motor_left;
    }

    if (in_report->valid_flag0 & 0x02) { // Lightbar update
        out_report.valid_flag1 |= (1 << 2); // Lightbar update
        out_report.lightbar_red = in_report->lightbar_red;
        out_report.lightbar_green = in_report->lightbar_green;
        out_report.lightbar_blue = in_report->lightbar_blue;
    }

    m_hid->hid_send_report(out_report);

    return 0;
}

void DualShockEmulator::setup_ep0(void)
{
    int ret;
    ep0_fd = ::open("/dev/uhid", O_RDWR);
    if (ep0_fd < 0) {
        std::cerr << "Failed to open /dev/uhid\n";
        return;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    strncpy((char*)ev.u.create2.name, "Wireless Controller", sizeof(ev.u.create2.name) - 1);
    strncpy((char*)ev.u.create2.phys, "ds4usb/virtual", sizeof(ev.u.create2.phys) - 1);
    ev.u.create2.rd_size = sizeof(descs);
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = 0x054c;
    ev.u.create2.product = 0x09cc;
    ev.u.create2.version = 0x0100;
    ev.u.create2.country = 0;
    memcpy(ev.u.create2.rd_data, descs, sizeof(descs));

    if (::write(ep0_fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to create uhid device\n";
        return;
    }

    m_functionfs_setup = true;

    /* Initialize endpoints and start I/O thread */
    init_ep();
    m_io_thread_running = true;
    m_io_thread = std::thread(&DualShockEmulator::do_io, this);
    m_io_thread.detach();
}

int DualShockEmulator::init_ep()
{
    // ep1 and ep2 are not used in uhid
    return 0;
}

void DualShockEmulator::do_io(void)
{
    struct uhid_event out_ev;

    while (m_io_thread_running)
    {
        // Read from uhid fd
        int ret = ::read(ep0_fd, &out_ev, sizeof(out_ev));
        if (ret > 0) {
            if (out_ev.type == UHID_OUTPUT) {
                uint8_t* uhid_out = out_ev.u.output.data;
                uint16_t size = out_ev.u.output.size;

                if (size == 32 && uhid_out[0] == 0x05) {
                    m_hid->hid_send_report_bt(uhid_out);
                }
            }
        }
    }
}

static bool s_running = true;
void signal_handler(int signum)
{
    std::cout << "Signal " << signum << " received, stopping...\n";
    s_running = false;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " /dev/hidrawX\n";
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        std::cerr << "Failed to set SIGINT handler\n";
        return 1;
    }

    DualShockEmulator *emulator = new DualShockEmulator();
    UsbHidEmulator *usb_hid = new UsbHidEmulator(emulator);

    usb_hid->set_hidraw_path(argv[1]);
    usb_hid->search_for_device();

    while (!emulator->dualsense_setup && s_running) {
        // Process events for connection methods
        usb_hid->process_device_events();
    }

    if (!emulator->get_hid()) {
        delete usb_hid;
        delete emulator;

        return 1;
    }

    if (emulator->get_hid()->type() == HidEmulator::HID_USB) {
        std::cout << "DualSense connected over USB\n";
    }

    // Keep the main thread alive
    std::cout << "Starting DualShock Emulator\n";
    emulator->init_ep();

    emulator->get_hid()->start(emulator->get_in_fd());

    while (emulator->dualsense_setup && s_running) {
        emulator->get_hid()->process_device_events();
    }

    std::cout << "Exiting emulator\n";

    delete emulator;

    return 0;
}
