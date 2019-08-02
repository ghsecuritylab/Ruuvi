/**
 * Copyright (c) 2018, Nordic Semiconductor ASA
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "sdk_config.h"
#include "sdk_macros.h"
#include "app_error.h"
#include "nrf_drv_rng.h"
#include "nfc_t2t_lib.h"
#include "nfc_ble_pair_msg.h"
#include "nrf_sdh_ble.h"
#include "nrf_crypto.h"
#include "nfc_pair_lib_m.h"
#include "nfc_central_m.h"
#include "nrf_ble_lesc.h"

#define NRF_LOG_MODULE_NAME   nfc_ble_pair
#if NFC_PAIR_LIB_M_LOG_ENABLED
  #define NRF_LOG_LEVEL       NFC_PAIR_LIB_M_LOG_LEVEL
  #define NRF_LOG_INFO_COLOR  NFC_PAIR_LIB_M_INFO_COLOR
  #define NRF_LOG_DEBUG_COLOR NFC_PAIR_LIB_M_DEBUG_COLOR
#else // NFC_BLE_PAIR_LIB_LOG_ENABLED
  #define NRF_LOG_LEVEL       0
#endif  // NFC_BLE_PAIR_LIB_LOG_ENABLED
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

// Verify bonding and keys distribution settings.
#if ((BLE_SEC_PARAM_BOND) &&           \
    !(BLE_SEC_PARAM_KDIST_OWN_ENC) &&  \
    !(BLE_SEC_PARAM_KDIST_OWN_ID) &&   \
    !(BLE_SEC_PARAM_KDIST_PEER_ENC) && \
    !(BLE_SEC_PARAM_KDIST_PEER_ID))
  #error \
    "At least one of the BLE_NFC_SEC_PARAM_KDIST flags must be set to 1 when bonding is enabled."
#endif

#define TK_MAX_NUM         1                                       /**< Maximum number of TK locations in an NDEF message buffer. */
#define NDEF_MSG_BUFF_SIZE 256                                     /**< Size of the buffer for the NDEF pairing message. */

volatile bool                  m_nfc_pherip = false;               /**< Flag indicating NFC peripheral pairing. */
static uint8_t                 m_ndef_msg_buf[NDEF_MSG_BUFF_SIZE]; /**< NFC tag NDEF message buffer. */
static ble_advdata_tk_value_t  m_oob_auth_key;                     /**< Temporary Key buffer used in OOB legacy pairing mode. */
static uint8_t               * m_tk_group[TK_MAX_NUM];             /**< Locations of TK in an NDEF message. */
static ble_gap_lesc_oob_data_t m_ble_lesc_oob_data;                /**< LESC OOB data used in LESC OOB pairing mode. */
static ble_gap_sec_params_t    m_sec_param;                        /**< Current Peer Manager secure parameters configuration. */

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context);

NRF_SDH_BLE_OBSERVER(m_ble_evt_observer, NFC_BLE_PAIR_LIB_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);


/**@brief Function for generating random values to a given buffer.
 *
 * @param[out] p_buff Buffer for random values.
 * @param[in]  size   Number of bytes to generate.
 *
 * @returns    Number of generated bytes.
 */
static uint8_t random_vector_generate(uint8_t * p_buff, uint8_t size)
{
    uint8_t    available;
    ret_code_t err_code = NRF_SUCCESS;

    nrf_drv_rng_bytes_available(&available);

    uint8_t length = (size < available) ? size : available;
    err_code = nrf_drv_rng_rand(p_buff, length);
    APP_ERROR_CHECK(err_code);

    return length;
}


/**@brief Function for printing a generated key to the log console.
 *
 * @param[in] length TK value length.
 */
static void random_vector_log(uint8_t length)
{
    NRF_LOG_INFO("TK Random Value:");

    for (uint32_t i = 0; i < length; i++)
    {
        NRF_LOG_RAW_INFO(" %02X", (int)m_oob_auth_key.tk[i]);
    }

    NRF_LOG_RAW_INFO("\r\n");
}


/**@brief Function for handling NFC events.
 *
 * @details Starts advertising and generates new OOB keys on the NFC_T2T_EVENT_FIELD_ON event.
 *
 * @param[in] p_context    Context for callback execution, not used in this callback implementation.
 * @param[in] event        Event generated by NFC lib HAL.
 * @param[in] p_data       Received/transmitted data or NULL, not used in this callback implementation.
 * @param[in] data_length  Size of the received/transmitted packet, not used in this callback implementation.
 */
static void nfc_callback(void          * p_context,
                         nfc_t2t_event_t event,
                         uint8_t const * p_data,
                         size_t          data_length)
{
    UNUSED_PARAMETER(p_context);
    UNUSED_PARAMETER(p_data);
    UNUSED_PARAMETER(data_length);

    ret_code_t err_code = NRF_SUCCESS;

    switch (event)
    {
        case NFC_T2T_EVENT_FIELD_ON:
            NRF_LOG_DEBUG("NFC_EVENT_FIELD_ON");

            {
                // Generate Authentication OOB Key and update NDEF message content.
                uint8_t length = random_vector_generate(m_oob_auth_key.tk, BLE_GAP_SEC_KEY_LEN);
                random_vector_log(length);
                err_code = nfc_tk_group_modifier_update(&m_oob_auth_key);
                APP_ERROR_CHECK(err_code);
                m_nfc_pherip = true;
            }
            break;

        case NFC_T2T_EVENT_FIELD_OFF:
            NRF_LOG_DEBUG("NFC_EVENT_FIELD_OFF");
            break;

        default:
            break;
    }
}


/**@brief Function for preparing the BLE pairing data for the NFC tag.
 *
 * @details This function does not stop and start the NFC tag data emulation.
 *
 * @param[in] mode Pairing mode for which the tag data will be prepared.
 *
 * @retval NRF_SUCCESS              If new tag pairing data has been set correctly.
 * @retval NRF_ERROR_INVALID_PARAM  If pairing mode is invalid.
 * @retval Other                    Other error codes might be returned depending on the used modules.
 */
ret_code_t nfc_ble_pair_data_set()
{
    ret_code_t err_code = NRF_SUCCESS;
    ble_gap_lesc_p256_pk_t const * p_pk_own;

    // Provide information about available buffer size to the encoding function.
    uint32_t ndef_msg_len = sizeof(m_ndef_msg_buf);

    // Get the local LESC public key
    p_pk_own = nrf_ble_lesc_public_key_get();

    // Generate LESC OOB data.
    err_code = sd_ble_gap_lesc_oob_data_get(BLE_CONN_HANDLE_INVALID,
                                            p_pk_own,
                                            &m_ble_lesc_oob_data);
    VERIFY_SUCCESS(err_code);

    // Encode NDEF message with Secure Simple Pairing OOB data - TK value and LESC Random and Confirmation Keys.
    err_code = nfc_ble_pair_msg_updatable_tk_encode(NFC_BLE_PAIR_MSG_BLUETOOTH_LE_SHORT,
                                                    &m_oob_auth_key,
                                                    &m_ble_lesc_oob_data,
                                                    m_ndef_msg_buf,
                                                    &ndef_msg_len,
                                                    m_tk_group,
                                                    TK_MAX_NUM);

    VERIFY_SUCCESS(err_code);

    // Update NFC tag data.
    err_code = nfc_t2t_payload_set(m_ndef_msg_buf, ndef_msg_len);

    return err_code;
}


ret_code_t nfc_ble_pair_init(void)
{
    ret_code_t err_code = NRF_SUCCESS;

    // Initialize RNG peripheral for authentication OOB data generation
    err_code = nrf_drv_rng_init(NULL);
    if (err_code != NRF_ERROR_INVALID_STATE &&
        err_code != NRF_ERROR_MODULE_ALREADY_INITIALIZED)
    {
        VERIFY_SUCCESS(err_code);
    }

    // Start NFC.
    err_code = nfc_t2t_setup(nfc_callback, NULL);
    VERIFY_SUCCESS(err_code);

    // Set proper NFC data.
    err_code = nfc_ble_pair_data_set();
    VERIFY_SUCCESS(err_code);

    return err_code;
}


/**@brief Function for Update the LESC OOB data
 *
 * @details The NFC Connection Handover message is updated with the LESC OOB data.
 *
 * @retval  NRF_SUCCESS If new tag pairing data has been set correctly.
 * @retval  Other       Other error codes might be returned depending on the used modules.
 */
static ret_code_t lesc_oob_update(uint16_t conn_handle)
{
    ble_gap_lesc_p256_pk_t  const * p_pk_own;
    ret_code_t                      err_code = NRF_SUCCESS;
    
    // Get the newly LESC public key
    p_pk_own = nrf_ble_lesc_public_key_get();
    
    // Generate LESC OOB data.
    err_code = sd_ble_gap_lesc_oob_data_get(conn_handle,
                                            p_pk_own,
                                            &m_ble_lesc_oob_data);
    VERIFY_SUCCESS(err_code);

    // Update NDEF message with new LESC OOB data.
    err_code = nfc_lesc_data_update(&m_ble_lesc_oob_data);
    VERIFY_SUCCESS(err_code);

    return NRF_SUCCESS;
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt       Event received from the BLE stack.
 * @param[in]   p_context       Context.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t                   err_code  = NRF_SUCCESS;
    ble_gap_evt_t        const * p_gap_evt = &p_ble_evt->evt.gap_evt;

    switch (p_ble_evt->header.evt_id)
    {

        // Upon authorization key request, reply with Temporary Key that was read from the NFC tag
        case BLE_GAP_EVT_AUTH_KEY_REQUEST:
            NRF_LOG_DEBUG("BLE_GAP_EVT_AUTH_KEY_REQUEST");

            // NFC central pair
            if (is_nfc_central_get())
            {
                ble_advdata_tk_value_t * oob_key;
                err_code = nfc_tk_value_get(&oob_key);
                APP_ERROR_CHECK(err_code);

                err_code = sd_ble_gap_auth_key_reply(p_gap_evt->conn_handle,
                                                     BLE_GAP_AUTH_KEY_TYPE_OOB,
                                                     oob_key->tk);
                APP_ERROR_CHECK(err_code);
            }
            // NFC peripheral pair
            else if (m_nfc_pherip)
            {
                err_code = sd_ble_gap_auth_key_reply(p_ble_evt->evt.gap_evt.conn_handle,
                                                     BLE_GAP_AUTH_KEY_TYPE_OOB,
                                                     m_oob_auth_key.tk);
                APP_ERROR_CHECK(err_code);
            }

            break;

        // Upon LESC Diffie_Hellman key request, set the OOB data if this is LESC OOB pairing
        case BLE_GAP_EVT_LESC_DHKEY_REQUEST:

            // If LESC OOB pairing is on, perform authentication with OOB data
            if (p_ble_evt->evt.gap_evt.params.lesc_dhkey_request.oobd_req)
            {
                uint16_t conn_handle = p_gap_evt->conn_handle;

                // If NFC central pair
                if (is_nfc_central_get())
                {
                    err_code = sd_ble_gap_lesc_oob_data_set(conn_handle,
                                                            NULL,
                                                            get_lesc_oob_peer_data());
                    APP_ERROR_CHECK(err_code);
                }
                else if (m_nfc_pherip)
                {
                    err_code = sd_ble_gap_lesc_oob_data_set(p_ble_evt->evt.gap_evt.conn_handle,
                                                            &m_ble_lesc_oob_data,
                                                            NULL);
                    APP_ERROR_CHECK(err_code);
                }
            }
            
            break;

        case BLE_GAP_EVT_AUTH_STATUS:
            // Key generation for next pair.
            err_code = lesc_oob_update(BLE_CONN_HANDLE_INVALID);
            APP_ERROR_CHECK(err_code);

            is_nfc_central_set(false);
            if (m_nfc_pherip)
            {
                m_nfc_pherip = false;
            }

            err_code = nfc_t2t_emulation_stop();
            APP_ERROR_CHECK(err_code);

            break;

        default:
            break;
    }
}


ret_code_t nfc_ble_pair_on_pm_params_req(pm_evt_t const * p_evt)
{
    ret_code_t err_code = NRF_SUCCESS;

    NRF_LOG_DEBUG("PM_EVT_CONN_SEC_PARAMS_REQ");

    if (is_nfc_central_get())
    {
        return err_code;
    }

    if (m_nfc_pherip)
    {
        ble_gap_sec_params_t const * const p_peer_sec_params =
            p_evt->params.conn_sec_params_req.p_peer_params;

        if (p_peer_sec_params->lesc)
        {
            NRF_LOG_DEBUG("LESC OOB mode flags set.");

            m_sec_param.mitm    = 1;
            m_sec_param.oob     = 0;
            m_sec_param.lesc    = 1;
            m_sec_param.io_caps = BLE_GAP_IO_CAPS_DISPLAY_YESNO;
        }
        else if (p_peer_sec_params->oob)
        {
            NRF_LOG_DEBUG("Legacy OOB mode flags set.");

            m_sec_param.mitm    = 1;
            m_sec_param.oob     = 1;
            m_sec_param.lesc    = 0;
            m_sec_param.io_caps = BLE_GAP_IO_CAPS_NONE;
        }
        else
        {
            return err_code;
        }

        m_sec_param.min_key_size = BLE_SEC_PARAM_MIN_KEY_SIZE;
        m_sec_param.max_key_size = BLE_SEC_PARAM_MAX_KEY_SIZE;
        m_sec_param.keypress     = BLE_SEC_PARAM_KEYPRESS;
        m_sec_param.bond         = BLE_SEC_PARAM_BOND;

        m_sec_param.kdist_own.enc  = BLE_SEC_PARAM_KDIST_OWN_ENC;
        m_sec_param.kdist_own.id   = BLE_SEC_PARAM_KDIST_OWN_ID;
        m_sec_param.kdist_peer.enc = BLE_SEC_PARAM_KDIST_PEER_ENC;
        m_sec_param.kdist_peer.id  = BLE_SEC_PARAM_KDIST_PEER_ID;

        // Reply with new security parameters to the Peer Manager.
        err_code = pm_conn_sec_params_reply(p_evt->conn_handle,
                                            &m_sec_param,
                                            p_evt->params.conn_sec_params_req.p_context);
    }

    return err_code;
}

