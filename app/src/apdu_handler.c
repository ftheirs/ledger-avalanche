/*******************************************************************************
 *   (c) 2018 - 2023 Zondax AG
 *   (c) 2016 Ledger
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 ********************************************************************************/

#include <os.h>
#include <os_io_seproxyhal.h>
#include <string.h>
#include <ux.h>

#include "actions.h"
#include "addr.h"
#include "app_main.h"
#include "coin.h"
#include "crypto.h"
#include "tx.h"
#include "view.h"
#include "view_internal.h"
#include "zxmacros.h"
#include "parser_common.h"
#include "rslib.h"
#include "eth_utils.h"

static bool tx_initialized = false;

bool
process_chunk_eth(__Z_UNUSED volatile uint32_t *tx, uint32_t rx);
void
extract_eth_path(uint32_t rx, uint32_t offset);

void extractHDPath(uint32_t rx, uint32_t offset) {
    tx_initialized = false;

    if (rx < offset + 1) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    uint8_t path_len = G_io_apdu_buffer[offset];
    uint8_t len_bytes = path_len * sizeof(uint32_t);

    if (path_len > HDPATH_LEN_DEFAULT || (rx - (offset + 1)) < len_bytes) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    memcpy(hdPath, G_io_apdu_buffer + offset + 1, len_bytes);
    // we need to pass this root path to rust,
    // later we can make rust ask for it but it would change other logic 
    // in the crypto module.
    // len_bytes + 1 to include the first byte that tells the number  
    // of elements in the path list
    _set_root_path(&G_io_apdu_buffer[offset], len_bytes + 1);
}

void
extract_eth_path(uint32_t rx, uint32_t offset)
{
    tx_initialized = false;

    // read one byte path len
    uint8_t path_len = G_io_apdu_buffer[offset];

    if (path_len > HDPATH_LEN_DEFAULT || path_len < 1){
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    if ((rx - offset - 1) < sizeof(uint32_t) * path_len) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    // first byte at OFFSET_DATA is the path len, so we skip this
    uint8_t *path_data = &G_io_apdu_buffer[offset + 1];

    // hw-app-eth serializes path as BE numbers
    for (uint8_t i = 0; i < path_len; i++) {
        hdPath[i] = U4BE(path_data, 0);
        path_data += sizeof(uint32_t);
    }

    // TODO: Do we need this checks for eth avax? 
    // check for proper chaind ids
    // const bool mainnet =
    //   hdPath[0] == HDPATH_ETH_0_DEFAULT && hdPath[1] == HDPATH_ETH_1_DEFAULT;
    //
    // if (!mainnet) {
    //     THROW(APDU_CODE_DATA_INVALID);
    // }

    // set the hdPath len
    hdPath_len = (uint32_t)path_len;

    // uint32_t len_bytes = path_len * sizeof(uint32_t);

    // we need to pass this root path to rust,
    // later we can make rust ask for it but it would change other logic 
    // in the crypto module.
    // len_bytes + 1 to include the first byte that tells the number  
    // of elements in the path list
    // _set_root_path(&G_io_apdu_buffer[offset], len_bytes + 1);
}

__Z_INLINE bool process_chunk(__Z_UNUSED volatile uint32_t *tx, uint32_t rx) {
    if (rx < OFFSET_DATA) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    uint32_t added;
    switch (G_io_apdu_buffer[OFFSET_PAYLOAD_TYPE]) {
        case P1_INIT:
            tx_initialize();
            tx_reset();
            extractHDPath(rx, OFFSET_DATA);
            tx_initialized = true;
            return false;
        case P1_ADD:
            if (!tx_initialized) {
                THROW(APDU_CODE_TX_NOT_INITIALIZED);
            }
            // we are appending the change_path list which 
            // needs to be removed before signing
            added = tx_append(&(G_io_apdu_buffer[OFFSET_DATA]), rx - OFFSET_DATA);
            if (added != rx - OFFSET_DATA) {
                tx_initialized = false;
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }
            return false;
        case P1_LAST:
            if (!tx_initialized) {
                THROW(APDU_CODE_TX_NOT_INITIALIZED);
            }
            added = tx_append(&(G_io_apdu_buffer[OFFSET_DATA]), rx - OFFSET_DATA);
            tx_initialized = false;
            if (added != rx - OFFSET_DATA) {
                tx_initialized = false;
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }
            tx_initialized = false;
            return true;
    }

    THROW(APDU_CODE_INVALIDP1P2);
}

bool
process_chunk_eth(__Z_UNUSED volatile uint32_t *tx, uint32_t rx)
{
    zemu_log("process_chunk_eth\n");
    const uint8_t payloadType = G_io_apdu_buffer[OFFSET_PAYLOAD_TYPE];

    if (G_io_apdu_buffer[OFFSET_P2] != 0) {
        zemu_log("invalid_p2???\n");
        THROW(APDU_CODE_INVALIDP1P2);
    }

    if (rx < OFFSET_DATA) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    uint64_t read = 0;
    uint64_t to_read = 0;
    uint64_t max_len = 0;

    uint8_t *data = &(G_io_apdu_buffer[OFFSET_DATA]);
    uint32_t len = rx - OFFSET_DATA;

    uint64_t added;
    switch (payloadType) {
        case P1_ETH_FIRST:
            zemu_log("P1_ETH_FIRST\n");
            tx_initialize();
            tx_reset();
            extract_eth_path(rx, OFFSET_DATA);
            // there is not warranties that the first chunk
            // contains the serialized path only;
            // so we need to offset the data to point to the first transaction
            // byte
            uint32_t path_len = sizeof(uint32_t) * hdPath_len;

            // plus the first offset data containing the path len
            data += path_len + 1;
            if (len < path_len) {
                THROW(APDU_CODE_WRONG_LENGTH);
            }

            // now process the chunk
            len -= path_len + 1;
            if (get_tx_rlp_len(data, len, &read, &to_read) != rlp_ok) {
                zemu_log("error rlp_decoding\n");
                THROW(APDU_CODE_DATA_INVALID);
            }

            // get remaining data len
            max_len = saturating_add(read, to_read);
            max_len = MIN(max_len, len);

            added = tx_append(data, max_len);
            if (added != max_len) {
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }

            tx_initialized = true;

            // if the number of bytes read and the number of bytes to read
            //  is the same as what we read...
            if ((saturating_add(read, to_read) - len) == 0) {
                zemu_log("received_all_data1!\n");
                return true;
            }
            return false;
        case P1_ETH_MORE:
            zemu_log("P1_ETH_MORE\n");
            if (!tx_initialized) {
                THROW(APDU_CODE_TX_NOT_INITIALIZED);
            }

            uint64_t buff_len = tx_get_buffer_length();
            uint8_t *buff_data = tx_get_buffer();

            if (get_tx_rlp_len(buff_data, buff_len, &read, &to_read) !=
                rlp_ok) {
                THROW(APDU_CODE_DATA_INVALID);
            }

            uint64_t rlp_read = buff_len - read;

            // either the entire buffer of the remaining bytes we expect
            uint64_t missing = to_read - rlp_read;
            max_len = len;

            if (missing < len)
                max_len = missing;

            added = tx_append(data, max_len);

            if (added != max_len) {
                tx_initialized = false;
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }

            // check if this chunk was the last one
            if (missing - len == 0) {
                tx_initialized = false;
                zemu_log("received_all_data2!\n");
                return true;
            }

            return false;
    }
    THROW(APDU_CODE_INVALIDP1P2);
}

__Z_INLINE void handleGetAddr(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("handleGetAddr\n");

    const uint8_t requireConfirmation = G_io_apdu_buffer[OFFSET_P1];
    zxerr_t zxerr = fill_address(flags, tx, rx, G_io_apdu_buffer, IO_APDU_BUFFER_SIZE);
    if (zxerr != zxerr_ok) {
        *tx = 0;
        THROW(APDU_CODE_DATA_INVALID);
    }
    if (requireConfirmation) {
        view_review_init(addr_getItem, addr_getNumItems, app_reply_address);
        view_review_show(REVIEW_ADDRESS);
        *flags |= IO_ASYNCH_REPLY;
        return;
    }

    THROW(APDU_CODE_OK);
}

__Z_INLINE void handleSignAvaxTx(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("handleSignAvaxTx\n");
    // This is the first transaction signing stage, where we receive the root path 
    // to be used for change_outputs and signers. so we need to tell process_chunk 
    // to parse it.
    if (!process_chunk(tx, rx)) {
        THROW(APDU_CODE_OK);
    }

    const char *error_msg = tx_avax_parse();

    CHECK_APP_CANARY()

    if (error_msg != NULL) {
        zemu_log(error_msg);
        const int error_msg_length = strnlen(error_msg, sizeof(G_io_apdu_buffer));
        memcpy(G_io_apdu_buffer, error_msg, error_msg_length);
        *tx += (error_msg_length);
        THROW(APDU_CODE_DATA_INVALID);
    }

    view_review_init(tx_getItem, tx_getNumItems, app_sign_tx);
    view_review_show(REVIEW_TXN);
    *flags |= IO_ASYNCH_REPLY;
}

__Z_INLINE void handleSignAvaxHash(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("handleSignAvaxHash\n");

    // we do not need to process_chunk 
    // all data was send in one go
    // and for now we are not ussing transaction buffer for this
    // if (!process_chunk(tx, rx, is_first_message)) {
    //     THROW(APDU_CODE_OK);
    // }

    // in this case we just received a path suffix 
    // we are supposed to use the previously stored 
    // root_path and hash
    if (G_io_apdu_buffer[OFFSET_P1] != FIRST_MESSAGE) {
        app_sign_hash();
    } else {
        // this is the sign_hash transaction 
        // we received in one go the root path 
        // and 32-bytes hash
        // so append it to our internal buffer and parse it
        tx_initialize();
        tx_reset();
        // this step is not really necessary
        extractHDPath(rx, OFFSET_DATA);

        uint16_t added = tx_append(&(G_io_apdu_buffer[OFFSET_DATA]), rx - OFFSET_DATA);

        if (added != rx - OFFSET_DATA) {
            THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
        }

        const char *error_msg = tx_avax_parse_hash();
        CHECK_APP_CANARY()

        if (error_msg != NULL) {

            const int error_msg_length = strnlen(error_msg, sizeof(G_io_apdu_buffer));
            memcpy(G_io_apdu_buffer, error_msg, error_msg_length);
            *tx += (error_msg_length);
            THROW(APDU_CODE_DATA_INVALID);
        }

        view_review_init(tx_getItem, tx_getNumItems, app_sign_hash);
        view_review_show(REVIEW_TXN);
    }

    *flags |= IO_ASYNCH_REPLY;
}

__Z_INLINE void handleSignAvaxMsg(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("handleSignAvaxMsg\n");

    // This is a message that comes with a root path and raw bytes to be signed
    if (!process_chunk(tx, rx)) {
        THROW(APDU_CODE_OK);
    }

    const char *error_msg = tx_avax_parse_msg();

    CHECK_APP_CANARY()

    if (error_msg != NULL) {
        zemu_log(error_msg);
        const int error_msg_length = strnlen(error_msg, sizeof(G_io_apdu_buffer));
        memcpy(G_io_apdu_buffer, error_msg, error_msg_length);
        *tx += (error_msg_length);
        THROW(APDU_CODE_DATA_INVALID);
    }

    view_review_init(tx_getItem, tx_getNumItems, app_sign_msg);
    view_review_show(REVIEW_TXN);
    *flags |= IO_ASYNCH_REPLY;
}

__Z_INLINE void handleSignEthTx(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("handleSignEthTx\n");

    if (!process_chunk_eth(tx, rx)) {
        THROW(APDU_CODE_OK);
    }

    const char *error_msg = tx_eth_parse();

    CHECK_APP_CANARY()

    if (error_msg != NULL) {
        zemu_log(error_msg);
        const int error_msg_length = strnlen(error_msg, sizeof(G_io_apdu_buffer));
        memcpy(G_io_apdu_buffer, error_msg, error_msg_length);
        *tx += (error_msg_length);
        THROW(APDU_CODE_DATA_INVALID);
    }

    view_review_init(tx_getItem, tx_getNumItems, app_sign_eth);
    view_review_show(REVIEW_TXN);
    *flags |= IO_ASYNCH_REPLY;
}


__Z_INLINE void handle_getversion(__Z_UNUSED volatile uint32_t *flags, volatile uint32_t *tx) {
    G_io_apdu_buffer[0] = 0;

#if defined(APP_TESTING)
    G_io_apdu_buffer[0] = 0x01;
#endif

    G_io_apdu_buffer[1] = (LEDGER_MAJOR_VERSION >> 8) & 0xFF;
    G_io_apdu_buffer[2] = (LEDGER_MAJOR_VERSION >> 0) & 0xFF;

    G_io_apdu_buffer[3] = (LEDGER_MINOR_VERSION >> 8) & 0xFF;
    G_io_apdu_buffer[4] = (LEDGER_MINOR_VERSION >> 0) & 0xFF;

    G_io_apdu_buffer[5] = (LEDGER_PATCH_VERSION >> 8) & 0xFF;
    G_io_apdu_buffer[6] = (LEDGER_PATCH_VERSION >> 0) & 0xFF;

    G_io_apdu_buffer[7] = !IS_UX_ALLOWED;

    G_io_apdu_buffer[8] = (TARGET_ID >> 24) & 0xFF;
    G_io_apdu_buffer[9] = (TARGET_ID >> 16) & 0xFF;
    G_io_apdu_buffer[10] = (TARGET_ID >> 8) & 0xFF;
    G_io_apdu_buffer[11] = (TARGET_ID >> 0) & 0xFF;

    *tx += 12;
    THROW(APDU_CODE_OK);
}

#if defined(APP_TESTING)
void handleTest(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) { THROW(APDU_CODE_OK); }
#endif

__Z_INLINE void avax_dispatch(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("AVAX Dispatch\n");
    switch (G_io_apdu_buffer[OFFSET_INS]) {
        case INS_GET_VERSION: {
            handle_getversion(flags, tx);
            break;
        }
        case AVX_GET_PUBLIC_KEY: {
            CHECK_PIN_VALIDATED()
            handleGetAddr(flags, tx, rx);
            break;
        }
        case AVX_SIGN: {
            CHECK_PIN_VALIDATED()
            handleSignAvaxTx(flags, tx, rx);
            break;
        }

        case AVX_SIGN_HASH: {
            CHECK_PIN_VALIDATED()
            handleSignAvaxHash(flags, tx, rx);

            break; 
        }

        case AVX_SIGN_MSG: {
            CHECK_PIN_VALIDATED()
            handleSignAvaxMsg(flags, tx, rx);

            break; 
        }
        default: {
            zemu_log("unknown_instruction***\n");
            THROW(APDU_CODE_INS_NOT_SUPPORTED);
        }
    }
}

__Z_INLINE void eth_dispatch(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    zemu_log("ETH Dispatch\n");
    switch (G_io_apdu_buffer[OFFSET_INS]) {
        case INS_ETH_SIGN: {
            handleSignEthTx(flags, tx, rx);
            break;
        }
        // case INS_ETH_GET_PUBLIC_KEY: {
        //     CHECK_PIN_VALIDATED()
        //     handleGetAddr(flags, tx, rx);
        //     break;
        // }
        // case INS_ETH_GET_APP_CONFIGURATION: {
        //     CHECK_PIN_VALIDATED()
        //     handleSignAvaxTx(flags, tx, rx);
        //     break;
        // }
        //
        // case INS_SET_PLUGIN: {
        //     CHECK_PIN_VALIDATED()
        //     handleSignAvaxHash(flags, tx, rx);
        //
        //     break; 
        // }
        //
        // case INS_PROVIDE_NFT_INFORMATION: {
        //     CHECK_PIN_VALIDATED()
        //     handleSignAvaxMsg(flags, tx, rx);
        //
        //     break; 
        // }
        //
        // case INS_ETH_PROVIDE_ERC20: {
        //     CHECK_PIN_VALIDATED()
        //     handleSignAvaxMsg(flags, tx, rx);
        //
        //     break; 
        // }
        //
        // case INS_SIGN_ETH_MSG: {
        //     CHECK_PIN_VALIDATED()
        //     handleSignAvaxMsg(flags, tx, rx);
        //
        //     break; 
        // }
        default: {
            zemu_log("unknown_instruction***\n");
            THROW(APDU_CODE_INS_NOT_SUPPORTED);
        }
    }
}

void handleApdu(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    volatile uint16_t sw = 0;
    zemu_log("handleApdu\n");

    BEGIN_TRY {
        TRY {

            if (rx < APDU_MIN_LENGTH) {
                THROW(APDU_CODE_WRONG_LENGTH);
            }

            if (G_io_apdu_buffer[OFFSET_CLA] == AVX_CLA) {
                zemu_log("calling avax_dispatch\n");
                avax_dispatch(flags, tx, rx);
            } else if (G_io_apdu_buffer[OFFSET_CLA] == ETH_CLA) {
                zemu_log_stack("calling eth_dispath\n");
                eth_dispatch(flags, tx, rx);
            } else {
                zemu_log("apdu_code not supported");
                THROW(APDU_CODE_CLA_NOT_SUPPORTED);
            }

            // Process non-avax instruction
            switch (G_io_apdu_buffer[OFFSET_INS]) {

#if defined(APP_TESTING)
                case INS_TEST: {
                    handleTest(flags, tx, rx);
                    THROW(APDU_CODE_OK);
                    break;
                }
#endif
                default:
                    THROW(APDU_CODE_INS_NOT_SUPPORTED);
            }
        }
        CATCH(EXCEPTION_IO_RESET) { THROW(EXCEPTION_IO_RESET); }
        CATCH_OTHER(e) {
            switch (e & 0xF000) {
                case 0x6000:
                case APDU_CODE_OK:
                    sw = e;
                    break;
                default:
                    sw = 0x6800 | (e & 0x7FF);
                    break;
            }
            G_io_apdu_buffer[*tx] = sw >> 8;
            G_io_apdu_buffer[*tx + 1] = sw & 0xFF;
            *tx += 2;
        }
        FINALLY {}
    }
    END_TRY;
}
