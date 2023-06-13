/**
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
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "configuration_manager.h"
#include "configuration_types.h"
#include "bqf.h"
#include "run.h"
#ifndef TEST_TARGET
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/usb_device.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/i2c.h"
#endif

/**
 * We have multiple copies of the device configuration. This is the factory
 * default configuration, it is static data in the firmware.
 * We also potentially have a user configuration stored at the end of flash
 * memory. And an in RAM working configuration.
 *
 * The idea is that when the device boots, it tries to use the user config
 * from the end of flash. If that is not present, or is invalid, we use this
 * default config instead.
 *
 * If the user sends an updated configuration over the USB port, it is stored
 * in RAM as a working configuration, and is used (until we lose power). If
 * the user issues a save command the working configuration is written to flash
 * and becomes the new user configuration. 
 */
static const default_configuration default_config = {
    .set_configuration = { SET_CONFIGURATION, sizeof(default_config) },
    .filters = {
        .filter = { FILTER_CONFIGURATION, sizeof(default_config.filters) },
        .f1 = { PEAKING,    {0},    38,   -19,  0.9 },
        .f2 = { LOWSHELF,   {0},    2900,   2,  0.7 },
        .f3 = { PEAKING,    {0},    430,    3,  3.5 },
        .f4 = { HIGHSHELF,  {0},    8400,   2,  0.7 },
        .f5 = { PEAKING,    {0},    4800,   3,    5 }
    },
    .preprocessing = { .header = { PREPROCESSING_CONFIGURATION, sizeof(default_config.preprocessing) }, -0.2f, false, {0} }
};

// Grab the last 4k page of flash for our configuration strutures.
#ifndef TEST_TARGET
static const size_t USER_CONFIGURATION_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
const uint8_t *user_configuration = (const uint8_t *) (XIP_BASE + USER_CONFIGURATION_OFFSET);
#endif
/**
 * TODO: For now, assume we always get a complete configuration but maybe we
 * should handle merging configurations where, for example, only a new
 * filter_configuration_tlv was received.
 */
#define CFG_BUFFER_SIZE 256
static uint8_t working_configuration[2][CFG_BUFFER_SIZE];
static uint8_t inactive_working_configuration = 0;
static uint8_t result_buffer[CFG_BUFFER_SIZE] = { U16_TO_U8S_LE(NOK), U16_TO_U8S_LE(0) };

static bool reload_config = false;
static uint16_t write_offset = 0;
static uint16_t read_offset = 0;

bool validate_filter_configuration(filter_configuration_tlv *filters)
{
    if (filters->header.type != FILTER_CONFIGURATION) {
        printf("Error! Not a filter TLV (%x)..\n", filters->header.type);
        return false;
    }
    uint8_t *ptr = (uint8_t *)filters->header.value;
    const uint8_t *end = (uint8_t *)filters + filters->header.length;
    int count = 0;
    while ((ptr + 4) < end) {
        const uint32_t type = *(uint32_t *)ptr;
        const uint16_t remaining = (uint16_t)(end - ptr);
        if (count++ > MAX_FILTER_STAGES) {
            printf("Error! Too many filters defined. (%d)\n", count);
            return false;
        }
        switch (type) {
        case LOWPASS:
        case HIGHPASS:
        case BANDPASSSKIRT:
        case BANDPASSPEAK:
        case NOTCH:
        case ALLPASS: {
            //filter2 *args = (filter2 *)ptr;
            //printf("Found Filter %d: %0.2f %0.2f\n", args->type, args->f0, args->Q);
            if (remaining < sizeof(filter2)) {
                printf("Error! Not enough data left for filter2 (%d)..\n", remaining);
                return false;
            }
            ptr += sizeof(filter2);
            break;
        }
        case PEAKING:
        case LOWSHELF:
        case HIGHSHELF: {
            //filter3 *args = (filter3 *)ptr;
            //printf("Found Filter %d: %0.2f %0.2f %0.2f\n", args->type, args->f0, args->db_gain, args->Q);
            if (remaining < sizeof(filter3)) {
                printf("Error! Not enough data left for filter3 (%d)..\n", remaining);
                return false;
            }
            ptr += sizeof(filter3);
            break;
        }
        default:
            printf("Unknown filter type\n");
            return false;
        }
    }
    if (ptr != end) {
        printf("Error! Did not consume the whole TLV (%p != %p)..\n", ptr, end);
        return false;
    }
    return true;
}

void apply_filter_configuration(filter_configuration_tlv *filters) {
    uint8_t *ptr = (uint8_t *)filters->header.value;
    const uint8_t *end = (uint8_t *)filters + filters->header.length;
    filter_stages = 0;

    while ((ptr + 4) < end) {
        const uint32_t type = *(uint32_t *)ptr;

        // If you reset the memory, you can hear it when you move the sliders on the UI,
        // is it perhaps OK to leave these and let the old values drop off over time?
        //bqf_memreset(&bqf_filters_mem_left[filter_stages]);
        //bqf_memreset(&bqf_filters_mem_right[filter_stages]);

        switch (type) {
            case LOWPASS: INIT_FILTER2(lowpass);
            case HIGHPASS: INIT_FILTER2(highpass);
            case BANDPASSSKIRT: INIT_FILTER2(bandpass_skirt);
            case BANDPASSPEAK: INIT_FILTER2(bandpass_peak);
            case NOTCH: INIT_FILTER2(notch);
            case ALLPASS: INIT_FILTER2(allpass);
            case PEAKING: INIT_FILTER3(peaking);
            case LOWSHELF: INIT_FILTER3(lowshelf);
            case HIGHSHELF: INIT_FILTER3(highshelf);
            default:
                break;
        }
        filter_stages++;
    }
}

bool validate_configuration(tlv_header *config) {
    uint8_t *ptr = NULL; 
    switch (config->type)
    {
        case SET_CONFIGURATION:
            ptr = (uint8_t *) config->value;
            break;
        case FLASH_HEADER: {
            flash_header_tlv* header = (flash_header_tlv*) config;
            if (header->magic != FLASH_MAGIC) {
                printf("Unexpected magic word (%x)\n", header->magic);
                return false;
            }
            if (header->version > CONFIG_VERSION) {
                printf("Config is too new (%d > %d)\n", header->version, CONFIG_VERSION);
                return false;
            }
            if (header->version < MINIMUM_CONFIG_VERSION) {
                printf("Config is too old (%d > %d)\n", header->version, MINIMUM_CONFIG_VERSION);
                return false;
            }
            ptr = (uint8_t *) header->tlvs;
            break;
        }
        default:
            printf("Unexpected Config type: %d\n", config->type);
            return false;
    }
    const uint8_t *end = (uint8_t *)config + config->length;
    while (ptr < end) {
        tlv_header* tlv = (tlv_header*) ptr;
        if (tlv->length < 4) {
            printf("Bad length... %d\n", tlv->length);
            return false;
        }
        switch (tlv->type) {
            case FILTER_CONFIGURATION:
                if (!validate_filter_configuration((filter_configuration_tlv*) tlv)) {
                    return false;
                }
                break;
            case PREPROCESSING_CONFIGURATION: {
                preprocessing_configuration_tlv* preprocessing_config = (preprocessing_configuration_tlv*) tlv;
                if (tlv->length != sizeof(preprocessing_configuration_tlv)) {
                    printf("Preprocessing size missmatch: %u != %zu\n", tlv->length, sizeof(preprocessing_configuration_tlv));
                    return false;
                }
                break;
            }
            case PCM3060_CONFIGURATION: {
                pcm3060_configuration_tlv* pcm3060_config = (pcm3060_configuration_tlv*) tlv;
                if (tlv->length != sizeof(pcm3060_configuration_tlv)) {
                    printf("PCM3060 config size missmatch: %u != %zu\n", tlv->length, sizeof(pcm3060_configuration_tlv));
                    return false;
                }
                break;
            }
            default:
                // Unknown TLVs are not invalid, just ignored.
                break;
        }
        ptr += tlv->length;
    }
    return true;
}

bool apply_configuration(tlv_header *config) {
    uint8_t *ptr = NULL; 
    switch (config->type)
    {
        case SET_CONFIGURATION:
            ptr = (uint8_t *) config->value;
            break;
        case FLASH_HEADER: {
            ptr = (uint8_t *) ((flash_header_tlv*) config)->tlvs;
            break;
        }
        default:
            printf("Unexpected Config type: %d\n", config->type);
            return false;
    }

    const uint8_t *end = (uint8_t *)config + config->length;
    while ((ptr + 4) < end) {
        tlv_header* tlv = (tlv_header*) ptr;
        switch (tlv->type) {
            case FILTER_CONFIGURATION:
                apply_filter_configuration((filter_configuration_tlv*) tlv);
                break;
            case PREPROCESSING_CONFIGURATION: {
                preprocessing_configuration_tlv* preprocessing_config = (preprocessing_configuration_tlv*) tlv;
                preprocessing.preamp = fix16_from_dbl(1.0 + preprocessing_config->preamp);
                preprocessing.reverse_stereo = preprocessing_config->reverse_stereo;
                break;
            }
            case PCM3060_CONFIGURATION: {
                pcm3060_configuration_tlv* pcm3060_config = (pcm3060_configuration_tlv*) tlv;
                audio_state.oversampling = pcm3060_config->oversampling;
                audio_state.phase = pcm3060_config->phase;
                audio_state.rolloff = pcm3060_config->rolloff;
                audio_state.de_emphasis = pcm3060_config->de_emphasis;
                break;
            }
            default:
                break;
        }
        ptr += tlv->length;
    }
    return true;
}

void load_config() {
#ifndef TEST_TARGET
    flash_header_tlv* hdr = (flash_header_tlv*) user_configuration;
    // Try to load data from flash
    if (validate_configuration((tlv_header*) user_configuration)) {
        apply_configuration((tlv_header*) user_configuration);
        return;
    }
#endif
    // If that is no good, use the default config
    apply_configuration((tlv_header*) &default_config);
}

#ifndef TEST_TARGET
bool __no_inline_not_in_flash_func(save_configuration)() {
    const uint8_t active_configuration = inactive_working_configuration ? 0 : 1;
    tlv_header* config = (tlv_header*) working_configuration[active_configuration];

    if (validate_configuration(config)) {
        power_down_dac();

        const size_t config_length = config->length - ((size_t)config->value - (size_t)config);
        // Write data to flash
        uint8_t flash_buffer[FLASH_PAGE_SIZE];
        flash_header_tlv* flash_header = (flash_header_tlv*) flash_buffer;
        flash_header->header.type = FLASH_HEADER;
        flash_header->header.length = sizeof(flash_header_tlv) + config_length;
        flash_header->magic = FLASH_MAGIC;
        flash_header->version = CONFIG_VERSION;
        memcpy((void*)(flash_header->tlvs), config->value, config_length);

        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(USER_CONFIGURATION_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(USER_CONFIGURATION_OFFSET, flash_buffer, FLASH_PAGE_SIZE);
        restore_interrupts(ints);

        power_up_dac();

        return true;
    }
    return false;
}

bool __no_inline_not_in_flash_func(factory_reset)() {
    power_down_dac();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(USER_CONFIGURATION_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    power_up_dac();
    return true;
}

bool process_cmd(tlv_header* cmd) {
    tlv_header* result = ((tlv_header*) result_buffer);
    switch (cmd->type) {
        case SET_CONFIGURATION:
            if (validate_configuration(cmd)) {
                inactive_working_configuration = inactive_working_configuration ? 0 : 1;
                reload_config = true;
                result->type = OK;
                result->length = 4;
                return true;
            }
            break;
        case SAVE_CONFIGURATION: {
            if (cmd->length == 4 && save_configuration()) {
                result->type = OK;
                result->length = 4;
                return true;
            }
            break;
        }
        case GET_ACTIVE_CONFIGURATION: {
            const uint8_t active_configuration = inactive_working_configuration ? 0 : 1;
            tlv_header* config = (tlv_header*) working_configuration[active_configuration];
            if (cmd->length == 4 && config->type == SET_CONFIGURATION && validate_configuration(config)) {
                result->type = OK;
                result->length = config->length;
                memcpy((void*)result->value, config->value, config->length - sizeof(tlv_header));  
                return true;
            }
            break;
        }
        case GET_STORED_CONFIGURATION: {
            if (cmd->length == 4) {
                flash_header_tlv* config = (flash_header_tlv*) user_configuration;
                // Assume the default config struct is good, so this can never fail.
                result->type = OK;
                // Try to load data from flash
                if (validate_configuration((tlv_header*)config)) {
                    const uint16_t payload_length = MIN(CFG_BUFFER_SIZE-sizeof(tlv_header), config->header.length - ((size_t)config->tlvs - (size_t)config));
                    result->length = payload_length + sizeof(tlv_header);
                    memcpy((void*)result->value, config->tlvs, payload_length);
                    return true;
                }
                result->length = default_config.set_configuration.length;
                memcpy((void*)result->value, default_config.set_configuration.value, default_config.set_configuration.length - sizeof(tlv_header));
                return true;
            }
            break;
        }
        case FACTORY_RESET: {
            if (cmd->length == 4 && factory_reset()) {
                flash_header_tlv flash_header = { 0 };
                result->type = OK;
                result->length = 4;
                return true;
            }
            break;
        }
        case GET_VERSION: {
            if (cmd->length == 4) {
                result->type = OK;
                result->length = 4 + sizeof(version_status_tlv);
                version_status_tlv* version = ((version_status_tlv*) result->value);
                version->header.type = VERSION_STATUS;
                version->header.length = sizeof(version_status_tlv);
                version->current_version = CONFIG_VERSION;
                version->minimum_supported_version = MINIMUM_CONFIG_VERSION;
                version->reserved = 0;
                return true;
            }
            break;
        }
    }
    result->type = NOK;
    result->length = 4;
    return false;
}

// This callback is called when the client sends a message to the device.
// We implement a simple messaging protocol. The client sends us a message that
// we consume here. All messages are constructed of TLV's (Type Length Value).
// In some cases the Value may be a set of TLV's. However, each message has an
// owning TLV, and its length determines the length of the transfer.
// Once we have consumed the whole message, we validate it and populate the result
// buffer with a TLV which we expect the client to read next.
void config_out_packet(struct usb_endpoint *ep) {
    struct usb_buffer *buffer = usb_current_out_packet_buffer(ep);
    //printf("config_out_packet %d\n", buffer->data_len);

    memcpy(&working_configuration[inactive_working_configuration][write_offset], buffer->data, buffer->data_len);
    write_offset += buffer->data_len;

    const uint16_t transfer_length = ((tlv_header*) working_configuration[inactive_working_configuration])->length;
    if (transfer_length && write_offset >= transfer_length) {
        // Command complete, fill the result buffer
        write_offset = 0;
        process_cmd((tlv_header*) working_configuration[inactive_working_configuration]);
        read_offset = 0;
    }

    usb_grow_transfer(ep->current_transfer, 1);
    usb_packet_done(ep);
}

// This callback is called when the client attempts to read data from the device.
// The client should have previously written a command which will have populated the
// result_buffer. The client should attempt to read 4 bytes (the Type and Length)
// then attempt to read the rest of the data once the length is known.
void config_in_packet(struct usb_endpoint *ep) {
    assert(ep->current_transfer);
    struct usb_buffer *buffer = usb_current_in_packet_buffer(ep);
    //printf("config_in_packet %d\n", buffer->data_len);
    assert(buffer->data_max >= 3);

    tlv_header* result = ((tlv_header*) result_buffer);
    const uint16_t transfer_length = ((tlv_header*) result_buffer)->length;
    const uint16_t packet_length = MIN(buffer->data_max, (uint16_t)(transfer_length - read_offset));
    memcpy(buffer->data, &result_buffer[read_offset], packet_length);
    buffer->data_len = packet_length;
    read_offset += packet_length;

    if (read_offset >= transfer_length) {
        // Done
        read_offset = 0;

        // If the client reads again, return nothing
        result->type = NOK;
        result->length = 0;
    }

    usb_grow_transfer(ep->current_transfer, 1);
    usb_packet_done(ep);
}

void apply_core1_config() {
    if (reload_config) {
        reload_config = false;
        const uint8_t active_configuration = inactive_working_configuration ? 0 : 1;
        apply_configuration((tlv_header*) working_configuration[active_configuration]);
    }
}
#endif