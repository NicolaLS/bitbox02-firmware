// Copyright 2023-2024 Shift Crypto AG
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "hww.h"
#include "memory/bitbox02_smarteeprom.h"
#include "usb/usb_packet.c"
#include "usb/usb_processing.c"
#include "usb/usb_processing.h"
#include "workflow/idle_workflow.h"
#include <fcntl.h>
#include <memory/memory.h>
#include <mock_memory.h>
#include <queue.h>
#include <random.h>
#include <rust/rust.h>
#include <sd.h>
#include <stdio.h>
#include <unistd.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

int data_len;
int commfd;

int get_usb_message_socket(uint8_t* input)
{
    return read(commfd, input, USB_HID_REPORT_OUT_SIZE);
}

void send_usb_message_socket(void)
{
    struct queue* q = queue_hww_queue();
    const uint8_t* data = queue_pull(q);
    if (data != NULL) {
        data_len = 256 * (int)data[5] + (int)data[6];
        if (!write(commfd, data, USB_HID_REPORT_OUT_SIZE)) {
            perror("ERROR, could not write to socket");
            exit(1);
        }
    }
}

void simulate_firmware_execution(const uint8_t* input)
{
    usb_packet_process((const USB_FRAME*)input);
    rust_workflow_spin();
    rust_async_usb_spin();
    usb_processing_process(usb_processing_hww());
}

int main(void)
{
    // BitBox02 simulation initialization
    usb_processing_init();
    usb_processing_set_send(usb_processing_hww(), send_usb_message_socket);
    printf("USB setup success\n");

    hww_setup();
    printf("HWW setup success\n");

    bool sd_success = sd_format();
    printf("Sd card setup %s\n", sd_success ? "success" : "failed");
    if (!sd_success) {
        perror("ERROR, sd card setup failed");
        return 1;
    }

    mock_memory_factoryreset();
    memory_interface_functions_t ifs = {
        .random_32_bytes = random_32_bytes_mcu,
    };
    bool memory_success = memory_setup(&ifs);
    printf("Memory setup %s\n", memory_success ? "success" : "failed");
    if (!memory_success) {
        perror("ERROR, memory setup failed");
        return 1;
    }

    smarteeprom_bb02_config();
    bitbox02_smarteeprom_init();
    idle_workflow_blocking();

    // Establish socket connection with client
    int portno = 15423;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        return 1;
    }
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    int serv_addr_len = sizeof(serv_addr);
    if (bind(sockfd, (struct sockaddr*)&serv_addr, serv_addr_len) < 0) {
        perror("ERROR binding socket");
        return 1;
    }
    if (listen(sockfd, 50) < 0) {
        perror("ERROR listening on socket");
        return 1;
    }
    while (1) {
        if ((commfd = accept(sockfd, (struct sockaddr*)&serv_addr, (socklen_t*)&serv_addr_len)) <
            0) {
            perror("accept");
            return 1;
        }
        printf("Socket connection setup success\n");

        // BitBox02 firmware loop
        uint8_t input[BUFFER_SIZE];
        int temp_len;
        while (1) {
            // Simulator polls for USB messages from client and then processes them
            if (!get_usb_message_socket(input)) break;
            simulate_firmware_execution(input);

            // If the USB message to be sent from firmware is bigger than one packet,
            // then the simulator sends the message in multiple packets. Packets use
            // HID format, just like the real USB messages.
            temp_len = data_len - (USB_HID_REPORT_OUT_SIZE - 7);
            while (temp_len > 0) {
                // When USB message processing function is called without a new
                // input, then it does not consume any packets but it still calls
                // the send function to send further USB messages
                usb_processing_process(usb_processing_hww());
                temp_len -= (USB_HID_REPORT_OUT_SIZE - 5);
            }
        }
        close(commfd);
        printf("Socket connection closed\n");
        printf("Waiting for new clients, CTRL+C to shut down the simulator\n");
    }
    return 0;
}