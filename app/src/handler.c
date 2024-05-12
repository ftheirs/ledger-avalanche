/*******************************************************************************
 *   (c) 2016 Ledger
 *   (c) 2018-2023 Zondax AG
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

#if defined(FEATURE_ETH)
// #include "globals.h"
// #include "io.h"
#include "shared_context.h"
#include "apdu_constants.h"
#include "common_ui.h"
#include "cx_errors.h"

#include "os_io_seproxyhal.h"

#include "glyphs.h"
#include "common_utils.h"

#include "swap_lib_calls.h"
#include "handle_swap_sign_transaction.h"
#include "handle_get_printable_amount.h"
#include "handle_check_address.h"
#include "commands_712.h"
#include "challenge.h"
#include "domain_name.h"
#include "lib_standard_app/crypto_helpers.h"


#include "handler.h"

// dispatcher_context_t G_dispatcher_context;

// taken from ethereum/src/main.c
void ui_idle(void);
chain_config_t *chainConfig = NULL;

uint32_t set_result_get_publicKey(void);
void finalizeParsing(bool);

tmpCtx_t tmpCtx;
txContext_t txContext;
tmpContent_t tmpContent;
dataContext_t dataContext;
strings_t strings;
cx_sha3_t global_sha3;

uint8_t appState;
uint16_t apdu_response_code;
bool G_called_from_swap;
bool G_swap_response_ready;
pluginType_t pluginType;

#ifdef HAVE_ETH2
uint32_t eth2WithdrawalIndex;
#include "withdrawal_index.h"
#endif

#include "ux.h"
ux_state_t G_ux;
bolos_ux_params_t G_ux_params;

const internalStorage_t N_storage_real;

#ifdef HAVE_NBGL
caller_app_t *caller_app = NULL;
#endif
// chain_config_t *chainConfig = NULL;

void reset_app_context() {
    // PRINTF("!!RESET_APP_CONTEXT\n");
    appState = APP_STATE_IDLE;
    G_called_from_swap = false;
    G_swap_response_ready = false;
    pluginType = OLD_INTERNAL;
#ifdef HAVE_ETH2
    eth2WithdrawalIndex = 0;
#endif
    memset((uint8_t *) &tmpCtx, 0, sizeof(tmpCtx));
    memset((uint8_t *) &txContext, 0, sizeof(txContext));
    memset((uint8_t *) &tmpContent, 0, sizeof(tmpContent));
}

void io_seproxyhal_send_status(uint32_t sw) {
    G_io_apdu_buffer[0] = ((sw >> 8) & 0xff);
    G_io_apdu_buffer[1] = (sw & 0xff);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
}
// clang-format off
// const command_descriptor_t COMMAND_DESCRIPTORS[] = {
//     {
//         .cla = CLA_ETH,
//         .ins = GET_EXTENDED_PUBKEY,
//         .handler = (command_handler_t)handler_get_extended_pubkey
//     },
//     {
//         .cla = CLA_ETH,
//         .ins = GET_WALLET_ADDRESS,
//         .handler = (command_handler_t)handler_get_wallet_address
//     },
//     {
//         .cla = CLA_ETH,
//         .ins = SIGN_PSBT,
//         .handler = (command_handler_t)handler_sign_psbt
//     },
//     {
//         .cla = CLA_ETH,
//         .ins = GET_MASTER_FINGERPRINT,
//         .handler = (command_handler_t)handler_get_master_fingerprint
//     },
//     {
//         .cla = CLA_ETH,
//         .ins = SIGN_MESSAGE,
//         .handler = (command_handler_t)handler_sign_message
//     },
// };
// clang-format on

extraInfo_t *getKnownToken(uint8_t *contractAddress) {
    union extraInfo_t *currentItem = NULL;
    // Works for ERC-20 & NFT tokens since both structs in the union have the
    // contract address aligned
    for (uint8_t i = 0; i < MAX_ITEMS; i++) {
        currentItem = (union extraInfo_t *) &tmpCtx.transactionContext.extraInfo[i].token;
        if (tmpCtx.transactionContext.tokenSet[i] &&
            (memcmp(currentItem->token.address, contractAddress, ADDRESS_LENGTH) == 0)) {
            PRINTF("Token found at index %d\n", i);
            return currentItem;
        }
    }

    return NULL;
}

const uint8_t *parseBip32(const uint8_t *dataBuffer, uint8_t *dataLength, bip32_path_t *bip32) {
    if (*dataLength < 1) {
        PRINTF("Invalid data\n");
        return NULL;
    }

    bip32->length = *dataBuffer;

    if (bip32->length < 0x1 || bip32->length > MAX_BIP32_PATH) {
        PRINTF("Invalid bip32\n");
        return NULL;
    }

    dataBuffer++;
    (*dataLength)--;

    if (*dataLength < sizeof(uint32_t) * (bip32->length)) {
        PRINTF("Invalid data\n");
        return NULL;
    }

    for (uint8_t i = 0; i < bip32->length; i++) {
        bip32->path[i] = U4BE(dataBuffer, 0);
        dataBuffer += sizeof(uint32_t);
        *dataLength -= sizeof(uint32_t);
    }

    return dataBuffer;
}

void handle_eth_apdu(volatile uint32_t *flags, volatile uint32_t *tx,
                     uint32_t rx, uint8_t *buffer, uint16_t bufferLen) {
    zemu_log("*****handle_eth_apdu\n");
    unsigned short sw = 0;

    BEGIN_TRY {
        TRY {
            if (buffer[OFFSET_CLA] != CLA) {
                THROW(0x6E00);
            }

            switch (buffer[OFFSET_INS]) {
                case INS_GET_PUBLIC_KEY:
                    memset(tmpCtx.transactionContext.tokenSet, 0, MAX_ITEMS);
                    handleGetPublicKey(buffer[OFFSET_P1],
                                       buffer[OFFSET_P2],
                                       buffer + OFFSET_CDATA,
                                       buffer[OFFSET_LC],
                                       flags,
                                       tx);
                    break;

                case INS_PROVIDE_ERC20_TOKEN_INFORMATION:
                    handleProvideErc20TokenInformation(buffer[OFFSET_P1],
                                                       buffer[OFFSET_P2],
                                                       buffer + OFFSET_CDATA,
                                                       buffer[OFFSET_LC],
                                                       flags,
                                                       tx);
                    break;

#ifdef HAVE_NFT_SUPPORT
                case INS_PROVIDE_NFT_INFORMATION:
                    handleProvideNFTInformation(buffer[OFFSET_P1],
                                                buffer[OFFSET_P2],
                                                buffer + OFFSET_CDATA,
                                                buffer[OFFSET_LC],
                                                flags,
                                                tx);
                    break;
#endif  // HAVE_NFT_SUPPORT

                case INS_SET_EXTERNAL_PLUGIN:
                    handleSetExternalPlugin(buffer[OFFSET_P1],
                                            buffer[OFFSET_P2],
                                            buffer + OFFSET_CDATA,
                                            buffer[OFFSET_LC],
                                            flags,
                                            tx);
                    break;

                case INS_SET_PLUGIN:
                    handleSetPlugin(buffer[OFFSET_P1],
                                    buffer[OFFSET_P2],
                                    buffer + OFFSET_CDATA,
                                    buffer[OFFSET_LC],
                                    flags,
                                    tx);
                    break;

                case INS_PERFORM_PRIVACY_OPERATION:
                    handlePerformPrivacyOperation(buffer[OFFSET_P1],
                                                  buffer[OFFSET_P2],
                                                  buffer + OFFSET_CDATA,
                                                  buffer[OFFSET_LC],
                                                  flags,
                                                  tx);
                    break;

                case INS_SIGN:
                    handleSign(buffer[OFFSET_P1],
                               buffer[OFFSET_P2],
                               buffer + OFFSET_CDATA,
                               buffer[OFFSET_LC],
                               flags,
                               tx);
                    break;

                case INS_GET_APP_CONFIGURATION:
                    handleGetAppConfiguration(buffer[OFFSET_P1],
                                              buffer[OFFSET_P2],
                                              buffer + OFFSET_CDATA,
                                              buffer[OFFSET_LC],
                                              flags,
                                              tx);
                    break;

                case INS_SIGN_PERSONAL_MESSAGE:
                    zemu_log("INS_SIGN_PERSONAL_MESSAGE\n");
                    memset(tmpCtx.transactionContext.tokenSet, 0, MAX_ITEMS);
                    *flags |= IO_ASYNCH_REPLY;
                    if (!handleSignPersonalMessage(buffer[OFFSET_P1],
                                                   buffer[OFFSET_P2],
                                                   buffer + OFFSET_CDATA,
                                                   buffer[OFFSET_LC])) {
                        reset_app_context();
                    }
                    break;

                case INS_SIGN_EIP_712_MESSAGE:
                    switch (buffer[OFFSET_P2]) {
                        case P2_EIP712_LEGACY_IMPLEM:
                            memset(tmpCtx.transactionContext.tokenSet, 0, MAX_ITEMS);
                            handleSignEIP712Message_v0(buffer[OFFSET_P1],
                                                       buffer[OFFSET_P2],
                                                       buffer + OFFSET_CDATA,
                                                       buffer[OFFSET_LC],
                                                       flags,
                                                       tx);
                            break;
#ifdef HAVE_EIP712_FULL_SUPPORT
                        case P2_EIP712_FULL_IMPLEM:
                            *flags |= IO_ASYNCH_REPLY;
                            handle_eip712_sign(buffer);
                            break;
#endif  // HAVE_EIP712_FULL_SUPPORT
                        default:
                            THROW(APDU_RESPONSE_INVALID_P1_P2);
                    }
                    break;

#ifdef HAVE_ETH2

                case INS_GET_ETH2_PUBLIC_KEY:
                    memset(tmpCtx.transactionContext.tokenSet, 0, MAX_ITEMS);
                    handleGetEth2PublicKey(buffer[OFFSET_P1],
                                           buffer[OFFSET_P2],
                                           buffer + OFFSET_CDATA,
                                           buffer[OFFSET_LC],
                                           flags,
                                           tx);
                    break;

                case INS_SET_ETH2_WITHDRAWAL_INDEX:
                    handleSetEth2WithdrawalIndex(buffer[OFFSET_P1],
                                                 buffer[OFFSET_P2],
                                                 buffer + OFFSET_CDATA,
                                                 buffer[OFFSET_LC],
                                                 flags,
                                                 tx);
                    break;

#endif

#ifdef HAVE_EIP712_FULL_SUPPORT
                case INS_EIP712_STRUCT_DEF:
                    *flags |= IO_ASYNCH_REPLY;
                    handle_eip712_struct_def(buffer);
                    break;

                case INS_EIP712_STRUCT_IMPL:
                    *flags |= IO_ASYNCH_REPLY;
                    handle_eip712_struct_impl(G_io_apdu_buffer);
                    break;

                case INS_EIP712_FILTERING:
                    *flags |= IO_ASYNCH_REPLY;
                    handle_eip712_filtering(buffer);
                    break;
#endif  // HAVE_EIP712_FULL_SUPPORT

#ifdef HAVE_DOMAIN_NAME
                case INS_ENS_GET_CHALLENGE:
                    handle_get_challenge();
                    break;

                case INS_ENS_PROVIDE_INFO:
                    handle_provide_domain_name(buffer[OFFSET_P1],
                                               buffer[OFFSET_P2],
                                               buffer + OFFSET_CDATA,
                                               buffer[OFFSET_LC]);
                    break;
#endif  // HAVE_DOMAIN_NAME

#if 0
        case 0xFF: // return to dashboard
          goto return_to_dashboard;
#endif

                default:
                    THROW(0x6D00);
                    break;
            }
        }
        CATCH(EXCEPTION_IO_RESET) {
            THROW(EXCEPTION_IO_RESET);
        }
        CATCH_OTHER(e) {
            bool quit_now = G_called_from_swap && G_swap_response_ready;
            switch (e & 0xF000) {
                case 0x6000:
                    // Wipe the transaction context and report the exception
                    sw = e;
                    reset_app_context();
                    break;
                case 0x9000:
                    // All is well
                    sw = e;
                    break;
                default:
                    // Internal error
                    sw = 0x6800 | (e & 0x7FF);
                    reset_app_context();
                    break;
            }
            // Unexpected exception => report
            buffer[*tx] = sw >> 8;
            buffer[*tx + 1] = sw;
            *tx += 2;

            // If we are in swap mode and have validated a TX, we send it and immediately quit
            if (quit_now) {
                if (io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, *tx) == 0) {
                    // In case of success, the apdu is sent immediately and eth exits
                    // Reaching this code means we encountered an error
                    finalize_exchange_sign_transaction(false);
                } else {
                    PRINTF("Unrecoverable\n");
                    os_sched_exit(-1);
                }
            }
        }
        FINALLY {
        }
    }
    END_TRY;
}

// Taken from ethereum app, this might be useful but need to double check if it fullfil our 
// purpose for signing eip712
/* Eth clones do not actually contain any logic, they delegate everything to the ETH application.
 * Start Eth in lib mode with the correct chain config
 */
__attribute__((noreturn)) void clone_main(libargs_t *args) {
    PRINTF("Starting in clone_main\n");
    BEGIN_TRY {
        TRY {
            unsigned int libcall_params[5];
            chain_config_t local_chainConfig;
            init_coin_config(&local_chainConfig);

            libcall_params[0] = (unsigned int) "Ethereum";
            libcall_params[1] = 0x100;
            libcall_params[3] = (unsigned int) &local_chainConfig;

            // Clone called by Exchange, forward the request to Ethereum
            if (args != NULL) {
                if (args->id != 0x100) {
                    os_sched_exit(0);
                }
                libcall_params[2] = args->command;
                libcall_params[4] = (unsigned int) args->get_printable_amount;
                os_lib_call((unsigned int *) &libcall_params);
                // Ethereum fulfilled the request and returned to us. We return to Exchange.
                os_lib_end();
            } else {
                // Clone called from Dashboard, start Ethereum
                libcall_params[2] = RUN_APPLICATION;
// On Stax, forward our icon to Ethereum
#ifdef HAVE_NBGL
                const char app_name[] = APPNAME;
                caller_app_t capp;
                nbgl_icon_details_t icon_details;
                uint8_t bitmap[sizeof(ICONBITMAP)];

                memcpy(&icon_details, &ICONGLYPH, sizeof(ICONGLYPH));
                memcpy(&bitmap, &ICONBITMAP, sizeof(bitmap));
                icon_details.bitmap = (const uint8_t *) bitmap;
                capp.name = app_name;
                capp.icon = &icon_details;
                libcall_params[4] = (unsigned int) &capp;
#else
                libcall_params[4] = 0;
#endif  // HAVE_NBGL
                os_lib_call((unsigned int *) &libcall_params);
                // Ethereum should not return to us
                os_sched_exit(-1);
            }
        }
        FINALLY {
        }
    }
    END_TRY;

    // os_lib_call will raise if Ethereum application is not installed. Do not try to recover.
    os_sched_exit(-1);
}



#endif
