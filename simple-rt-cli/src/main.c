/*
 * SimpleRT: Reverse tethering utility for Android
 * Copyright (C) 2016 Konstantin Menyaev
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "linux-adk.h"
#include "network.h"

/* detached per-device working thread */
static void *connection_thread_proc(void *param)
{
    accessory_t *acc = param;
    int ret = 0, transferred = 0;
    uint8_t acc_buf[ACC_BUF_SIZE];

    /* FIXME: wait for hotplug_callback released */
    usleep(100);

    /* init accessory from new connected device and wait for present */
    if (!is_accessory_present(acc)) {
        init_accessory(acc);
        goto end;
    }

    puts("accessory connected!");

    /* Claiming first (accessory) interface from the opened device */
    ret = libusb_claim_interface(acc->handle, AOA_ACCESSORY_INTERFACE);
    if (ret != 0) {
        fprintf(stderr, "Error claiming interface: %s\n", libusb_strerror(ret));
        goto end;
    }

    acc->is_running = true;

    while (acc->is_running) {
        ret = libusb_bulk_transfer(acc->handle, AOA_ACCESSORY_EP_IN,
                acc_buf, sizeof(acc_buf), &transferred, ACC_TIMEOUT);
        if (ret < 0) {
            if (ret == LIBUSB_ERROR_TIMEOUT) {
                continue;
            } else {
                fprintf(stderr, "Acc thread: bulk transfer error: %s\n",
                        libusb_strerror(ret));
                break;
            }
        } else {
            if (send_network_packet(acc_buf, transferred) < 0) {
                fprintf(stderr, "Send network packet faield!\n");
                break;
            }
        }
    }

end:
    acc->is_running = false;
    free_accessory(acc);
    return NULL;
}

static int hotplug_callback(struct libusb_context *ctx,
        struct libusb_device *dev,
        libusb_hotplug_event event,
        void * arg)
{
    if (event != LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        fprintf(stderr, "Unknown libusb_hotplug_event: %d\n", event);
        return 0;
    }

    accessory_t * acc = new_accessory(dev);

    pthread_t th;
    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attrs, connection_thread_proc, acc);

    return 0;
}

int main(int argc, char *argv[])
{
    int rc = 0;
    libusb_hotplug_callback_handle callback_handle;

    libusb_init(NULL);

    if (argc > 1) {
        const char *param = argv[1];

        if (strcmp(param, "-d") == 0) {
            puts("debug mode enabled");
            libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_DEBUG);
        } else if (strcmp(param, "-h") == 0) {
            puts("usage: sudo simple-rt [-h -d]");
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown param: %s\n", param);
            return EXIT_FAILURE;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "Run app as root!\n");
        return EXIT_FAILURE;
    }

    if (!start_network()) {
        fprintf(stderr, "Unable to start network!\n");
        return EXIT_FAILURE;
    }

    rc = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, 0,
            LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback, NULL, &callback_handle);

    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "Error creating a hotplug callback\n");
        return EXIT_FAILURE;
    }

    puts("SimpleRT started!");

    while (true) {
        libusb_handle_events_completed(NULL, NULL);
        usleep(1);
    }

    stop_network();
    libusb_hotplug_deregister_callback(NULL, callback_handle);
    libusb_exit(NULL);

    return EXIT_SUCCESS;
}

