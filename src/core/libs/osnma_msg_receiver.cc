/*!
 * \file osnma_msg_receiver.cc
 * \brief GNU Radio block that processes Galileo OSNMA data received from
 * Galileo E1B telemetry blocks. After successful decoding, sends the content to
 * the PVT block.
 * \author Carles Fernandez-Prades, 2023. cfernandez(at)cttc.es
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2010-2023  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */


#include "osnma_msg_receiver.h"
#include "Galileo_OSNMA.h"
#include "gnss_crypto.h"
#include "gnss_satellite.h"
#include "osnma_dsm_reader.h"       // for OSNMA_DSM_Reader
#include "pvt_interface.h"
#include <glog/logging.h>           // for DLOG
#include <gnuradio/io_signature.h>  // for gr::io_signature::make
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <typeinfo>  // for typeid

#if HAS_GENERIC_LAMBDA
#else
#include <boost/bind/bind.hpp>
#endif

#if PMT_USES_BOOST_ANY
#include <boost/any.hpp>
namespace wht = boost;
#else
#include <any>
namespace wht = std;
#endif


osnma_msg_receiver_sptr osnma_msg_receiver_make(const std::string& pemFilePath, const std::string& merkleFilePath)
{
    return osnma_msg_receiver_sptr(new osnma_msg_receiver(pemFilePath, merkleFilePath));
}


osnma_msg_receiver::osnma_msg_receiver(
    const std::string& pemFilePath,
    const std::string& merkleFilePath) : gr::block("osnma_msg_receiver",
                                             gr::io_signature::make(0, 0, 0),
                                             gr::io_signature::make(0, 0, 0))
{
    d_dsm_reader = std::make_unique<OSNMA_DSM_Reader>();
    d_crypto = std::make_unique<Gnss_Crypto>(pemFilePath, merkleFilePath);
    d_old_mack_message.set_capacity(10);
    //  register OSNMA input message port from telemetry blocks
    this->message_port_register_in(pmt::mp("OSNMA_from_TLM"));
    this->set_msg_handler(pmt::mp("OSNMA_from_TLM"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_osnma(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&osnma_msg_receiver::msg_handler_osnma, this, boost::placeholders::_1));
#else
        boost::bind(&osnma_msg_receiver::msg_handler_osnma, this, _1));
#endif
#endif
    // register OSNMA input message port from PVT block
    this->message_port_register_in(pmt::mp("pvt_to_osnma"));
    this->set_msg_handler(pmt::mp("pvt_to_osnma"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_pvt_to_osnma(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&osnma_msg_receiver::msg_handler_pvt_to_osnma, this, boost::placeholders::_1));
#else
        boost::bind(&osnma_msg_receiver::msg_handler_pvt_to_osnma, this, _1));
#endif
#endif
    // register OSNMA output message port to PVT block
    this->message_port_register_out(pmt::mp("OSNMA_to_PVT"));
}

void osnma_msg_receiver::msg_handler_pvt_to_osnma(const pmt::pmt_t& msg)
{
    try
        {
            d_receiver_time = wht::any_cast<std::time_t>(pmt::any_ref(msg)); // C: TODO - check if this is the correct way to get the time from the PVT block
        }
    catch (const pmt::exception& e)
        {
            LOG(WARNING) << "osnma_msg_receiver pmt exception: " << e.what();
        }
}
void osnma_msg_receiver::msg_handler_osnma(const pmt::pmt_t& msg)
{
    // requires mutex with msg_handler_osnma function called by the scheduler
    gr::thread::scoped_lock lock(d_setlock);
    try
        {
            const size_t msg_type_hash_code = pmt::any_ref(msg).type().hash_code();
            if (msg_type_hash_code == typeid(std::shared_ptr<OSNMA_msg>).hash_code())
                {
                    const auto nma_msg = wht::any_cast<std::shared_ptr<OSNMA_msg>>(pmt::any_ref(msg));
                    const auto sat = Gnss_Satellite(std::string("Galileo"), nma_msg->PRN);
                    std::cout << "Galileo OSNMA: Subframe received starting at "
                              << "WN="
                              << nma_msg->WN_sf0
                              << ", TOW="
                              << nma_msg->TOW_sf0
                              << ", from satellite "
                              << sat
                              << std::endl;

                    // compare local time with OSNMA subframe time
                    d_GST_SIS = nma_msg->TOW_sf0 + nma_msg->WN_sf0 * 604800; // TODO - unsure about this operation and of the -24 seconds,...
                    double_t T_L = 15; // TODO - to define the maximum allowed time difference between local time and OSNMA subframe time
                    if(abs(d_GST_SIS - d_receiver_time) <= T_L)
                        {
                            process_osnma_message(nma_msg);
                        }
                    else
                        {
                            LOG(WARNING) << "OSNMA: Subframe received with time difference greater than " << T_L << " seconds";
                        }
                }
            else
                {
                    LOG(WARNING) << "osnma_msg_receiver received an unknown object type!";
                }
        }
    catch (const wht::bad_any_cast& e)
        {
            LOG(WARNING) << "osnma_msg_receiver Bad any_cast: " << e.what();
        }

    //  Send the resulting decoded NMA data (if available) to PVT
    if (d_new_data == true) // TODO where is it set to true?
        {
            auto osnma_data_ptr = std::make_shared<OSNMA_data>(d_osnma_data);
            this->message_port_pub(pmt::mp("OSNMA_to_PVT"), pmt::make_any(osnma_data_ptr));
            d_new_data = false;
            // d_osnma_data = OSNMA_data();
            DLOG(INFO) << "NMA info sent to the PVT block through the OSNMA_to_PVT async message port";
        }
}


void osnma_msg_receiver::process_osnma_message(const std::shared_ptr<OSNMA_msg>& osnma_msg)
{
    read_nma_header(osnma_msg->hkroot[0]);
    read_dsm_header(osnma_msg->hkroot[1]);
    read_dsm_block(osnma_msg);
    read_mack_block(osnma_msg);
}


void osnma_msg_receiver::read_nma_header(uint8_t nma_header)
{
    d_osnma_data.d_nma_header.nmas = d_dsm_reader->get_nmas(nma_header);
    d_osnma_data.d_nma_header.cid = d_dsm_reader->get_cid(nma_header);
    d_osnma_data.d_nma_header.cpks = d_dsm_reader->get_cpks(nma_header);
    d_osnma_data.d_nma_header.reserved = d_dsm_reader->get_nma_header_reserved(nma_header);
}


void osnma_msg_receiver::read_dsm_header(uint8_t dsm_header)
{
    d_osnma_data.d_dsm_header.dsm_id = d_dsm_reader->get_dsm_id(dsm_header);
    d_osnma_data.d_dsm_header.dsm_block_id = d_dsm_reader->get_dsm_block_id(dsm_header);  // BID
    LOG(WARNING) << "OSNMA: DSM_ID=" << static_cast<uint32_t>(d_osnma_data.d_dsm_header.dsm_id);
    LOG(WARNING) << "OSNMA: DSM_BID=" << static_cast<uint32_t>(d_osnma_data.d_dsm_header.dsm_block_id);
    std::cout << "Galileo OSNMA: Received block " << static_cast<uint32_t>(d_osnma_data.d_dsm_header.dsm_block_id)
              << " from DSM_ID " << static_cast<uint32_t>(d_osnma_data.d_dsm_header.dsm_id)
              << std::endl;
}

/*
 * accumulates dsm messages until completeness, then calls process_dsm_message
 * */
void osnma_msg_receiver::read_dsm_block(const std::shared_ptr<OSNMA_msg>& osnma_msg)
{
    size_t index = 0;
    for (const auto* it = osnma_msg->hkroot.cbegin() + 2; it != osnma_msg->hkroot.cend(); ++it)
        {
            d_dsm_message[d_osnma_data.d_dsm_header.dsm_id][SIZE_DSM_BLOCKS_BYTES * d_osnma_data.d_dsm_header.dsm_block_id + index] = *it;
            index++;
        }
    if (d_osnma_data.d_dsm_header.dsm_block_id == 0)
        {
            // Get number of blocks in message
            uint8_t nb = d_dsm_reader->get_number_blocks_index(d_dsm_message[d_osnma_data.d_dsm_header.dsm_id][0]);
            uint16_t number_of_blocks = 0;
            if (d_osnma_data.d_dsm_header.dsm_id < 12)
                {
                    // DSM-KROOT Table 7
                    const auto it = OSNMA_TABLE_7.find(nb);
                    if (it != OSNMA_TABLE_7.cend())
                        {
                            number_of_blocks = it->second.first;
                        }
                }
            else if (d_osnma_data.d_dsm_header.dsm_id >= 12 && d_osnma_data.d_dsm_header.dsm_id < 16)
                {
                    // DSM-PKR Table 3
                    const auto it = OSNMA_TABLE_3.find(nb);
                    if (it != OSNMA_TABLE_3.cend())
                        {
                            number_of_blocks = it->second.first;
                        }
                }

            d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id] = number_of_blocks;
            LOG(WARNING) << "OSNMA: number_of_blocks=" << static_cast<uint32_t>(number_of_blocks);
            if (number_of_blocks == 0)
                {
                    // Something is wrong, start over
                    LOG(WARNING) << "OSNMA: Wrong number of blocks, start over";
                    d_dsm_message[d_osnma_data.d_dsm_header.dsm_id] = std::array<uint8_t, 256>{};
                    d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id] = std::array<uint8_t, 16>{};
                }
        }
    // Annotate bid
    d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id][d_osnma_data.d_dsm_header.dsm_block_id] = 1;

    std::cout << "Galileo OSNMA: Available blocks for DSM_ID " << static_cast<uint32_t>(d_osnma_data.d_dsm_header.dsm_id) << ": [ ";
    if (d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id] == 0)
        {
            for (auto id_received : d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id])
                {
                    if (id_received == 0)
                        {
                            std::cout << "- ";
                        }
                    else
                        {
                            std::cout << "X ";
                        }
                }
        }
    else
        {
            for (uint8_t k = 0; k < d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id]; k++)
                {
                    if (d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id][k] == 0)
                        {
                            std::cout << "- ";
                        }
                    else
                        {
                            std::cout << "X ";
                        }
                }
        }
    std::cout << "]" << std::endl;

    // is message complete? -> Process DSM message
    if ((d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id] != 0) &&
        (d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id] == std::accumulate(d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id].cbegin(), d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id].cend(), 0)))
        {
            std::vector<uint8_t> dsm_msg(std::size_t(d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id]) * SIZE_DSM_BLOCKS_BYTES, 0);
            for (uint32_t i = 0; i < d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id]; i++)
                {
                    for (size_t j = 0; j < SIZE_DSM_BLOCKS_BYTES; j++)
                        {
                            dsm_msg[i * SIZE_DSM_BLOCKS_BYTES + j] = d_dsm_message[d_osnma_data.d_dsm_header.dsm_id][i * SIZE_DSM_BLOCKS_BYTES + j];
                        }
                }
            d_dsm_message[d_osnma_data.d_dsm_header.dsm_id] = std::array<uint8_t, 256>{};
            d_dsm_id_received[d_osnma_data.d_dsm_header.dsm_id] = std::array<uint8_t, 16>{};
            process_dsm_message(dsm_msg, osnma_msg);
        }
}

/*
 * case DSM-Kroot:
 * - computes the padding and compares with received message
 * - if successful, tries to verify the digital signature
 * case DSM-PKR:
 * - calls verify_dsm_pkr to verify the public key
 * */
void osnma_msg_receiver::process_dsm_message(const std::vector<uint8_t>& dsm_msg, const std::shared_ptr<OSNMA_msg>& osnma_msg)
{
    if (d_osnma_data.d_dsm_header.dsm_id < 12)
        {
            LOG(WARNING) << "OSNMA: DSM-KROOT message received.";
            // DSM-KROOT message
            d_osnma_data.d_dsm_kroot_message.nb_dk = d_dsm_reader->get_number_blocks_index(dsm_msg[0]);
            d_osnma_data.d_dsm_kroot_message.pkid = d_dsm_reader->get_pkid(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.cidkr = d_dsm_reader->get_cidkr(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.reserved1 = d_dsm_reader->get_dsm_reserved1(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.hf = d_dsm_reader->get_hf(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.mf = d_dsm_reader->get_mf(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.ks = d_dsm_reader->get_ks(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.ts = d_dsm_reader->get_ts(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.maclt = d_dsm_reader->get_maclt(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.reserved = d_dsm_reader->get_dsm_reserved(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.wn_k = d_dsm_reader->get_wn_k(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.towh_k = d_dsm_reader->get_towh_k(dsm_msg);
            d_osnma_data.d_dsm_kroot_message.alpha = d_dsm_reader->get_alpha(dsm_msg);

            const uint16_t l_lk_bytes = d_dsm_reader->get_lk_bits(d_osnma_data.d_dsm_kroot_message.ks) / 8;
            d_osnma_data.d_dsm_kroot_message.kroot = d_dsm_reader->get_kroot(dsm_msg, l_lk_bytes);

            std::string hash_function = d_dsm_reader->get_hash_function(d_osnma_data.d_dsm_kroot_message.hf);
            uint16_t l_ds_bits = 0;
            const auto it = OSNMA_TABLE_15.find(hash_function);
            if (it != OSNMA_TABLE_15.cend())
                {
                    l_ds_bits = it->second;
                }
            const uint16_t l_ds_bytes = l_ds_bits / 8;
            d_osnma_data.d_dsm_kroot_message.ds = std::vector<uint8_t>(l_ds_bytes, 0);
            for (uint16_t k = 0; k < l_ds_bytes; k++)
                {
                    d_osnma_data.d_dsm_kroot_message.ds[k] = dsm_msg[13 + l_lk_bytes + k];
                }
            const uint16_t l_dk_bits = d_dsm_reader->get_l_dk_bits(d_osnma_data.d_dsm_kroot_message.nb_dk);
            const uint16_t l_dk_bytes = l_dk_bits / 8;
            const uint16_t l_pdk_bytes = (l_dk_bytes - 13 - l_lk_bytes - l_ds_bytes);
            d_osnma_data.d_dsm_kroot_message.p_dk = std::vector<uint8_t>(l_pdk_bytes, 0);
            for (uint16_t k = 0; k < l_pdk_bytes; k++)
                {
                    d_osnma_data.d_dsm_kroot_message.p_dk[k] = dsm_msg[13 + l_lk_bytes + l_ds_bytes + k];
                }

            const uint16_t check_l_dk = 104 * std::ceil(1.0 + static_cast<float>((l_lk_bytes * 8.0) + l_ds_bits) / 104.0);
            if (l_dk_bits != check_l_dk)
                {
                    std::cout << "Galileo OSNMA: Failed length reading" << std::endl;
                }
            else
                {
                    // validation of padding
                    const uint16_t size_m = 13 + l_lk_bytes;
                    std::vector<uint8_t> MSG;
                    MSG.reserve(size_m + l_ds_bytes + 1);
                    MSG.push_back(osnma_msg->hkroot[0]); // C: NMA header
                    for (uint16_t i = 1; i < size_m; i++)
                        {
                            MSG.push_back(dsm_msg[i]);
                        }
                    std::vector<uint8_t> message = MSG; // C: MSG == M || DS from ICD. Eq. 7
                    for (uint16_t k = 0; k < l_ds_bytes; k++)
                        {
                            MSG.push_back(d_osnma_data.d_dsm_kroot_message.ds[k]);
                        }

                    std::vector<uint8_t> hash;
                    if (d_osnma_data.d_dsm_kroot_message.hf == 0)  // Table 8.
                        {
                            hash = d_crypto->computeSHA256(MSG);
                        }
                    else if (d_osnma_data.d_dsm_kroot_message.hf == 2)
                        {
                            hash = d_crypto->computeSHA3_256(MSG);
                        }
                    else
                        {
                            hash = std::vector<uint8_t>(32);
                        }
                    // truncate hash
                    std::vector<uint8_t> p_dk_truncated;
                    p_dk_truncated.reserve(l_pdk_bytes);
                    for (uint16_t i = 0; i < l_pdk_bytes; i++)
                        {
                            p_dk_truncated.push_back(hash[i]);
                        }
                    // Check that the padding bits received match the computed values
                    if (d_osnma_data.d_dsm_kroot_message.p_dk == p_dk_truncated)
                        {
                            bool authenticated = d_crypto->verify_signature(message, d_osnma_data.d_dsm_kroot_message.ds);
                            LOG(WARNING) << "OSNMA: DSM-KROOT message received ok.";
                            std::cout << "Galileo OSNMA: KROOT with CID=" << static_cast<uint32_t>(d_osnma_data.d_nma_header.cid)
                                      << ", PKID=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.pkid)
                                      << ", WN=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.wn_k)
                                      << ", TOW=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.towh_k) * 3600;
                            if (authenticated)
                                {
                                    std::cout << " authenticated" << std::endl; // C: proceed with Tesla chain key verification.
                                }
                            else
                                {
                                    std::cout << " validated" << std::endl; // C: Kroot not verified => retrieve it again
                                }
                            std::cout << "Galileo OSNMA: NMA Status is " << d_dsm_reader->get_nmas_status(d_osnma_data.d_nma_header.nmas) << ", "
                                      << "Chain in force is " << static_cast<uint32_t>(d_osnma_data.d_nma_header.cid) << ", "
                                      << "Chain and Public Key Status is " << d_dsm_reader->get_cpks_status(d_osnma_data.d_nma_header.cpks) << std::endl;
                        }
                    else
                        {
                            std::cout << "Galileo OSNMA: Error computing padding bits." << std::endl;
                        }
                }
        }
    else if (d_osnma_data.d_dsm_header.dsm_id >= 12 && d_osnma_data.d_dsm_header.dsm_id < 16)
        {
            LOG(WARNING) << "OSNMA: DSM-PKR message received.";
            // Save DSM-PKR message
            d_osnma_data.d_dsm_pkr_message.nb_dp = d_dsm_reader->get_number_blocks_index(dsm_msg[0]);
            d_osnma_data.d_dsm_pkr_message.mid = d_dsm_reader->get_mid(dsm_msg);
            for (int k = 0; k > 128; k++)
                {
                    d_osnma_data.d_dsm_pkr_message.itn[k] = dsm_msg[k + 1];
                }
            d_osnma_data.d_dsm_pkr_message.npkt = d_dsm_reader->get_npkt(dsm_msg);
            d_osnma_data.d_dsm_pkr_message.npktid = d_dsm_reader->get_npktid(dsm_msg);

            uint32_t l_npk = 0;
            const auto it = OSNMA_TABLE_5.find(d_osnma_data.d_dsm_pkr_message.npkt);
            if (it != OSNMA_TABLE_5.cend())
                {
                    const auto it2 = OSNMA_TABLE_6.find(it->second);
                    if (it2 != OSNMA_TABLE_6.cend())
                        {
                            l_npk = it2->second / 8;
                        }
                }
            uint32_t l_dp = dsm_msg.size();
            if (d_osnma_data.d_dsm_pkr_message.npkt == 4)
                {
                    LOG(WARNING) << "OSNMA: OAM received";
                    l_npk = l_dp - 130;  // bytes
                }

            d_osnma_data.d_dsm_pkr_message.npk = std::vector<uint8_t>(l_npk, 0);  // ECDSA Public Key
            for (uint32_t k = 0; k > l_npk; k++)
                {
                    d_osnma_data.d_dsm_pkr_message.npk[k] = dsm_msg[k + 130];
                }

            uint32_t l_pd = l_dp - 130 - l_npk;
            uint32_t check_l_dp = 104 * std::ceil(static_cast<float>(1040.0 + l_npk * 8.0) / 104.0);
            if (l_dp != check_l_dp)
                {
                    std::cout << "Galileo OSNMA: Failed length reading" << std::endl;
                }
            else
                {
                    d_osnma_data.d_dsm_pkr_message.p_dp = std::vector<uint8_t>(l_pd, 0);
                    for (uint32_t k = 0; k < l_pd; k++)
                        {
                            d_osnma_data.d_dsm_pkr_message.p_dp[k] = dsm_msg[l_dp - l_pd + k];
                        }
                    // std::vector<uint8_t> mi;  //  (NPKT + NPKID + NPK)
                    std::cout << "Galileo OSNMA: DSM-PKR with CID=" << static_cast<uint32_t>(d_osnma_data.d_nma_header.cid)
                              << ", PKID=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.pkid)
                              << ", WN=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.wn_k)
                              << ", TOW=" << static_cast<uint32_t>(d_osnma_data.d_dsm_kroot_message.towh_k) * 3600
                              << " received" << std::endl;
                    // C: NPK verification against Merkle tree root.
                    if (!d_public_key_verified)
                        {
                            bool verification = verify_dsm_pkr(d_osnma_data.d_dsm_pkr_message);
                            if (verification)
                                {
                                    d_public_key_verified = true;
                                    d_crypto->set_public_key(d_osnma_data.d_dsm_pkr_message.npk);
                                }
                        }

                }
        }
    else
        {
            // Reserved message?
            LOG(WARNING) << "OSNMA Reserved message received";
            // d_osnma_data = OSNMA_data();
        }
    d_number_of_blocks[d_osnma_data.d_dsm_header.dsm_id] = 0;
}

// reads Mack message
void osnma_msg_receiver::read_mack_block(const std::shared_ptr<OSNMA_msg>& osnma_msg)
{
    uint32_t index = 0;
    for (uint32_t value : osnma_msg->mack)
        {
            d_mack_message[index] = static_cast<uint8_t>((value & 0xFF000000) >> 24);
            d_mack_message[index + 1] = static_cast<uint8_t>((value & 0x00FF0000) >> 16);
            d_mack_message[index + 2] = static_cast<uint8_t>((value & 0x0000FF00) >> 8);
            d_mack_message[index + 3] = static_cast<uint8_t>(value & 0x000000FF);
            index = index + 4;
        }
    // compute time of subrame and kroot time of applicability, used in read_mack_body and process_mack_message
    // TODO - find a better placement
    d_GST_SIS = osnma_msg->TOW_sf0 + osnma_msg->WN_sf0 * 604800; // TODO - unsure about this operation and of the -24 seconds,...
    d_GST_0 = d_osnma_data.d_dsm_kroot_message.towh_k + 604800 * d_osnma_data.d_dsm_kroot_message.wn_k;
    d_GST_Sf = d_GST_0 + 30 * std::floor((d_GST_SIS-d_GST_0)/30); // Eq. 3 R.G.
    if (d_osnma_data.d_dsm_kroot_message.ts != 0) // C: 4 ts <  ts < 10
        {
            read_mack_header();
            read_mack_body();
            process_mack_message(osnma_msg);
        }
}


void osnma_msg_receiver::read_mack_header()
{ // C: TODO - still to review computations.
    uint8_t lt_bits = 0;
    const auto it = OSNMA_TABLE_11.find(d_osnma_data.d_dsm_kroot_message.ts);
    if (it != OSNMA_TABLE_11.cend())
        {
            lt_bits = it->second;
        }
    if (lt_bits == 0)
        {
            return; // C: TODO if Tag length is 0, what is the action? no verification possible of NavData for sure.
        }
    uint16_t macseq = 0;
    uint8_t cop = 0;
    uint64_t first_lt_bits = static_cast<uint64_t>(d_mack_message[0]) << (lt_bits - 8);
    first_lt_bits += (static_cast<uint64_t>(d_mack_message[1]) << (lt_bits - 16));
    if (lt_bits == 20)
        {
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[1] & 0xF0) >> 4);
            macseq += (static_cast<uint16_t>(d_mack_message[1] & 0x0F) << 8);
            macseq += static_cast<uint16_t>(d_mack_message[2]);
            cop += ((d_mack_message[3] & 0xF0) >> 4);
        }
    else if (lt_bits == 24)
        {
            first_lt_bits += static_cast<uint64_t>(d_mack_message[2]);
            macseq += (static_cast<uint16_t>(d_mack_message[3]) << 4);
            macseq += (static_cast<uint16_t>(d_mack_message[4] & 0xF0) >> 4);
            cop += (d_mack_message[4] & 0x0F);
        }
    else if (lt_bits == 28)
        {
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[2]) << 4);
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[3] & 0xF0) >> 4);
            macseq += (static_cast<uint16_t>(d_mack_message[3] & 0x0F) << 8);
            macseq += (static_cast<uint16_t>(d_mack_message[4]));
            cop += ((d_mack_message[5] & 0xF0) >> 4);
        }
    else if (lt_bits == 32)
        {
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[2]) << 8);
            first_lt_bits += static_cast<uint64_t>(d_mack_message[3]);
            macseq += (static_cast<uint16_t>(d_mack_message[4]) << 4);
            macseq += (static_cast<uint16_t>(d_mack_message[5] & 0xF0) >> 4);
            cop += (d_mack_message[5] & 0x0F);
        }
    else if (lt_bits == 40)
        {
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[2]) << 16);
            first_lt_bits += (static_cast<uint64_t>(d_mack_message[3]) << 8);
            first_lt_bits += static_cast<uint64_t>(d_mack_message[4]);
            macseq += (static_cast<uint16_t>(d_mack_message[5]) << 4);
            macseq += (static_cast<uint16_t>(d_mack_message[6] & 0xF0) >> 4);
            cop += (d_mack_message[6] & 0x0F);
        }
    d_osnma_data.d_mack_message.header.tag0 = first_lt_bits;
    d_osnma_data.d_mack_message.header.macseq = macseq;
    d_osnma_data.d_mack_message.header.cop = cop;
}


void osnma_msg_receiver::read_mack_body()
{
    uint8_t lt_bits = 0;
    const auto it = OSNMA_TABLE_11.find(d_osnma_data.d_dsm_kroot_message.ts);
    if (it != OSNMA_TABLE_11.cend())
        {
            lt_bits = it->second;
        }
    if (lt_bits == 0)
        {
            return;
        }
    uint16_t lk_bits = 0;
    const auto it2 = OSNMA_TABLE_10.find(d_osnma_data.d_dsm_kroot_message.ks);
    if (it2 != OSNMA_TABLE_10.cend())
        {
            lk_bits = it2->second;
        }
    if (lk_bits == 0)
        {
            return;
        }
    uint16_t nt = std::floor((480.0 - float(lk_bits)) / (float(lt_bits) + 16.0)); // C: compute number of tags
    d_osnma_data.d_mack_message.tag_and_info = std::vector<MACK_tag_and_info>(nt - 1);
    for (uint16_t k = 0; k < (nt - 1); k++) // C: retrieve Tag&Info
        {
            uint64_t tag = 0;
            uint8_t PRN_d = 0;
            uint8_t ADKD = 0;
            uint8_t cop = 0;
            if (lt_bits == 20)
                {
                    const uint16_t step = std::ceil(4.5 * k);
                    if (k % 2 == 0)
                        {
                            tag += (static_cast<uint64_t>((d_mack_message[3 + step] & 0x0F)) << 16);
                            tag += (static_cast<uint64_t>(d_mack_message[4 + step]) << 8);
                            tag += static_cast<uint64_t>(d_mack_message[5 + step]);
                            PRN_d += d_mack_message[6 + step];
                            ADKD += ((d_mack_message[7 + step] & 0xF0) >> 4);
                            cop += (d_mack_message[7 + step] & 0x0F);
                            if (k == (nt - 2))
                                {
                                    d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                                    for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                        {
                                            d_osnma_data.d_mack_message.key[j] = d_mack_message[8 + step + j];
                                        }
                                }
                        }
                    else
                        {
                            tag += (static_cast<uint64_t>(d_mack_message[3 + step]) << 12);
                            tag += (static_cast<uint64_t>(d_mack_message[4 + step]) << 4);
                            tag += (static_cast<uint64_t>((d_mack_message[5 + step] & 0xF0)) >> 4);
                            PRN_d += (d_mack_message[5 + step] & 0x0F) << 4;
                            PRN_d += (d_mack_message[6 + step] & 0xF0) >> 4;
                            ADKD += (d_mack_message[6 + step] & 0x0F);
                            cop += (d_mack_message[7 + step] & 0xF0) >> 4;
                            if (k == (nt - 2))
                                {
                                    d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                                    for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                        {
                                            d_osnma_data.d_mack_message.key[j] = ((d_mack_message[7 + step + j] & 0x0F) << 4) + ((d_mack_message[8 + step + j] & 0xF0) >> 4);
                                        }
                                }
                        }
                }
            else if (lt_bits == 24)
                {
                    tag += (static_cast<uint64_t>((d_mack_message[5 + k * 5])) << 16);
                    tag += (static_cast<uint64_t>((d_mack_message[6 + k * 5])) << 8);
                    tag += static_cast<uint64_t>(d_mack_message[7 + k * 5]);
                    PRN_d += d_mack_message[8 + k * 5];
                    ADKD += ((d_mack_message[9 + k * 5] & 0xF0) >> 4);
                    cop += (d_mack_message[9 + k * 5] & 0x0F);
                    if (k == (nt - 2))
                        {
                            d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                            for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                {
                                    d_osnma_data.d_mack_message.key[j] = d_mack_message[10 + k * 5 + j];
                                }
                        }
                }
            else if (lt_bits == 28)
                {
                    const uint16_t step = std::ceil(5.5 * k);
                    if (k % 2 == 0)
                        {
                            tag += (static_cast<uint64_t>((d_mack_message[5 + step] & 0x0F)) << 24);
                            tag += (static_cast<uint64_t>(d_mack_message[6 + step]) << 16);
                            tag += (static_cast<uint64_t>(d_mack_message[7 + step]) << 8);
                            tag += static_cast<uint64_t>(d_mack_message[8 + step]);
                            PRN_d += d_mack_message[9 + step];
                            ADKD += ((d_mack_message[10 + step] & 0xF0) >> 4);
                            cop += (d_mack_message[10 + step] & 0x0F);
                            if (k == (nt - 2))
                                {
                                    d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                                    for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                        {
                                            d_osnma_data.d_mack_message.key[j] = d_mack_message[11 + step + j];
                                        }
                                }
                        }
                    else
                        {
                            tag += (static_cast<uint64_t>((d_mack_message[5 + step])) << 20);
                            tag += (static_cast<uint64_t>((d_mack_message[6 + step])) << 12);
                            tag += (static_cast<uint64_t>((d_mack_message[7 + step])) << 4);
                            tag += (static_cast<uint64_t>((d_mack_message[8 + step] & 0xF0)) >> 4);
                            PRN_d += ((d_mack_message[8 + step] & 0x0F) << 4);
                            PRN_d += ((d_mack_message[9 + step] & 0xF0) >> 4);
                            ADKD += (d_mack_message[9 + step] & 0x0F);
                            cop += ((d_mack_message[10 + step] & 0xF0) >> 4);
                            if (k == (nt - 2))
                                {
                                    d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                                    for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                        {
                                            d_osnma_data.d_mack_message.key[j] = ((d_mack_message[10 + step + j] & 0x0F) << 4) + ((d_mack_message[11 + step + j] & 0xF0) >> 4);
                                        }
                                }
                        }
                }
            else if (lt_bits == 32)
                {
                    tag += (static_cast<uint64_t>((d_mack_message[6 + k * 6])) << 24);
                    tag += (static_cast<uint64_t>((d_mack_message[7 + k * 6])) << 16);
                    tag += (static_cast<uint64_t>((d_mack_message[8 + k * 6])) << 8);
                    tag += static_cast<uint64_t>(d_mack_message[9 + k * 6]);
                    PRN_d += d_mack_message[10 + k * 6];
                    ADKD += ((d_mack_message[11 + k * 6] & 0xF0) >> 4);
                    cop += (d_mack_message[11 + k * 6] & 0x0F);
                    if (k == (nt - 2))
                        {
                            d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                            for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                {
                                    d_osnma_data.d_mack_message.key[j] = d_mack_message[12 + k * 6 + j];
                                }
                        }
                }
            else if (lt_bits == 40)
                {
                    tag += (static_cast<uint64_t>((d_mack_message[7 + k * 7])) << 32);
                    tag += (static_cast<uint64_t>((d_mack_message[8 + k * 7])) << 24);
                    tag += (static_cast<uint64_t>((d_mack_message[9 + k * 7])) << 16);
                    tag += (static_cast<uint64_t>((d_mack_message[10 + k * 7])) << 8);
                    tag += static_cast<uint64_t>(d_mack_message[11 + k * 7]);
                    PRN_d += d_mack_message[12 + k * 7];
                    ADKD += ((d_mack_message[13 + k * 7] & 0xF0) >> 4);
                    cop += (d_mack_message[13 + k * 7] & 0x0F);
                    if (k == (nt - 2))
                        {
                            d_osnma_data.d_mack_message.key = std::vector<uint8_t>(d_osnma_data.d_dsm_kroot_message.kroot.size());
                            for (size_t j = 0; j < d_osnma_data.d_dsm_kroot_message.kroot.size(); j++)
                                {
                                    d_osnma_data.d_mack_message.key[j] = d_mack_message[14 + k * 7 + j];
                                }
                        }
                }
            d_osnma_data.d_mack_message.tag_and_info[k].tag = tag;
            d_osnma_data.d_mack_message.tag_and_info[k].tag_info.PRN_d = PRN_d;
            d_osnma_data.d_mack_message.tag_and_info[k].tag_info.ADKD = ADKD;
            d_osnma_data.d_mack_message.tag_and_info[k].tag_info.cop = cop;
        }

    // retrieve tesla key
    uint8_t start_index_bytes = ( 480 - (lt_bits + 16) - (nt - 1) * ( lt_bits + 16 ) - 1 ) / 8; // includes -1 to start at [i-1]
    uint8_t last_index_bytes = ( start_index_bytes * 8 + lk_bits ) / 8;
    uint8_t key_index_bytes = 0;
    for (uint8_t i = start_index_bytes; i < last_index_bytes ; i++, key_index_bytes++)
        {
            d_osnma_data.d_mack_message.key[key_index_bytes] = d_mack_message[i];
        }

    // compute number of hashes required
    uint8_t num_of_hashes_needed = (d_GST_Sf - d_GST_0) / 30 + 1;
    uint32_t GST_SFi = d_GST_Sf;
    std::vector<uint8_t> K_II = d_osnma_data.d_mack_message.key;
    std::vector<uint8_t> K_I; // result of the recursive hash operations
    uint8_t size_hash_f = d_osnma_data.d_dsm_kroot_message.ks / 8;
    // compute the current tesla key , GST_SFi and K_II change in each iteration
    for (uint8_t i = 1; i < num_of_hashes_needed ; i++)
        {
            // build message digest m = (K_I+1 || GST_SFi || alpha)
            std::vector<uint8_t> msg(sizeof(K_II) + sizeof(GST_SFi) + sizeof(d_osnma_data.d_dsm_kroot_message.alpha));
            std::copy(K_II.begin(),K_II.end(),msg.begin());

            msg.push_back((d_GST_Sf & 0xFF000000) >> 24);
            msg.push_back((d_GST_Sf & 0x00FF0000) >> 16);
            msg.push_back((d_GST_Sf & 0x0000FF00) >> 8);
            msg.push_back(d_GST_Sf & 0x000000FF);

            for (uint8_t k = 5; k >= 0;k--)
                {
                    // TODO: static extracts the MSB in case from larger to shorter int?
                    msg.push_back(static_cast<uint8_t>((d_osnma_data.d_dsm_kroot_message.alpha >> (i * 8)) & 0xFF)); // extract first 6 bytes of alpha.
                }

            // compute hash
            std::vector<uint8_t> hash;
            if (d_osnma_data.d_dsm_kroot_message.hf == 0)  // Table 8.
                {
                    hash = d_crypto->computeSHA256(msg);
                }
            else if (d_osnma_data.d_dsm_kroot_message.hf == 2)
                {
                    hash = d_crypto->computeSHA3_256(msg);
                }
            else
                {
                    hash = std::vector<uint8_t>(32);
                }
            // truncate hash
            K_I.reserve(size_hash_f); // TODO - case hash function has 512 bits
            for (uint16_t i = 0; i < size_hash_f; i++)
                {
                    K_I.push_back(hash[i]);
                }

            // set parameters
            GST_SFi -= 30; // next SF time is the actual minus 30 seconds
            K_II = K_I; // next key is the actual one
            K_I.clear(); // empty the actual one for a new computation
        }
    // compare computed current key against received key
    bool result_comparison; // TODO - not needed?
    if(K_I.size() != d_osnma_data.d_mack_message.key.size())
        {
            std::cout << "Galileo OSNMA: Error during tesla key verification. " << std::endl;
        }
    if (K_II == d_osnma_data.d_mack_message.key)
        {
            result_comparison = true;
            std::cout << "Galileo OSNMA: tesla key verified successfully " << std::endl;
            // TODO - propagate result
            // TODO - save current tesla key as latest one?
            // TODO - Tags Sequence Verification: check ADKD[i] follows MACLT sequence


        }
    else

        {
            result_comparison = false;
            std::cout << "Galileo OSNMA: Error during tesla key verification. " << std::endl;
        }
}


void osnma_msg_receiver::process_mack_message(const std::shared_ptr<OSNMA_msg>& osnma_msg)
{
    d_old_mack_message.push_back(d_osnma_data.d_mack_message); // last 10 MACKs are needed to be stored as per ICD
    // populate d_nav_data with three classes of osnma_msg
    d_osnma_data.d_nav_data.EphemerisData = osnma_msg->EphemerisData;
    d_osnma_data.d_nav_data.IonoData = osnma_msg->IonoData;
    d_osnma_data.d_nav_data.UtcData = osnma_msg->UtcModelData;
    d_osnma_data.d_nav_data.generate_eph_iono_vector2();
    d_osnma_data.d_nav_data.generate_utc_vector();
    d_old_navdata_buffer.push_back(d_osnma_data.d_nav_data); // last 10 NavData messages are needed to be stored as per ICD
    // MACSEQ validation - case no FLX Tags

    // retrieve data to verify MACK tags
    uint8_t msg {0};
    uint8_t nt {0};
    std::vector<std::string> sq1{};
    std::vector<std::string> sq2{};
    const auto it = OSNMA_TABLE_16.find(d_osnma_data.d_dsm_kroot_message.maclt);
    if (it != OSNMA_TABLE_16.cend())
        {
            uint8_t msg = it->second.msg;
            uint8_t nt = it->second.nt;
            std::vector<std::string> sq1 = it->second.sequence1;
            std::vector<std::string> sq2 = it->second.sequence2;
        }
    if (msg == 0)
        {
          return;
        }
    std::vector<std::string> sequence;
    if (d_GST_Sf % 60 == 0)
        {
            sequence = sq1;
        }
    else if (d_GST_Sf % 60 == 30)
        {
            sequence = sq2;
        }
    else
        {
            std::cout << "Galileo OSNMA: Mismatch in the GST verification => should end in 30 or 60 seconds but it dit not." << std::endl;
        }
    // compare ADKD of Mack tags with MACLT defined ADKDs
    if(d_osnma_data.d_mack_message.tag_and_info.size() != sq1.size())
        {
          std::cout << "Galileo OSNMA: Number of retrieved tags does not match MACLT sequence size!" << std::endl;
          return;
        }
    std::vector<uint8_t> flxTags {};
    std::string tempADKD;
    for (uint8_t i = 0; i < d_osnma_data.d_mack_message.tag_and_info.size(); i++)
        {
            tempADKD = sequence[i];
            if(tempADKD == "FLX")
                {
                    flxTags.push_back(i); // C: just need to save the index in the sequence
                }
            else if(d_osnma_data.d_mack_message.tag_and_info[i].tag_info.ADKD != std::stoi(sequence[i]))
                {                    std::cout << "Galileo OSNMA: Unsuccessful verification of received ADKD against MAC Look-up table. " << std::endl;
                    return; // C: suffices one incorrect to abort and not process the rest of the tags
                }
        }

    // MACSEQ verification

    // Case no tags defined as FLX in the MACLT
    std::vector<uint8_t> m(5 + flxTags.size()); // C: ICD - Eq. 22
    m[0] = static_cast<uint8_t>(osnma_msg->PRN);  // PRN_A
    m[1] = static_cast<uint8_t>((d_GST_Sf & 0xFF000000) >> 24);
    m[2] = static_cast<uint8_t>((d_GST_Sf & 0x00FF0000) >> 16);
    m[3] = static_cast<uint8_t>((d_GST_Sf & 0x0000FF00) >> 8);
    m[4] = static_cast<uint8_t>(d_GST_Sf & 0x000000FF);

    // Case tags flexible
    for (uint8_t i = 0; i < flxTags.size() ; i++)
        {
            m[i+5] = d_osnma_data.d_mack_message.tag_and_info[flxTags[i]].tag_info.PRN_d;
            m[i+6] = d_osnma_data.d_mack_message.tag_and_info[flxTags[i]].tag_info.ADKD << 4 | d_osnma_data.d_mack_message.tag_and_info[flxTags[i]].tag_info.cop;
        }
    std::vector<uint8_t> applicable_key;
    // if ADKD=12, pick d_old_mack_message.front() if d_old_mack_message[10] is full
    // otherwise pick d_old_mack_message.back()
    applicable_key = d_old_mack_message.back().key;
    std::vector<uint8_t> mac;
    if (d_osnma_data.d_dsm_kroot_message.mf == 0) // C: HMAC-SHA-256
        {
            mac = d_crypto->computeHMAC_SHA_256(applicable_key, m);
        }
    else if (d_osnma_data.d_dsm_kroot_message.mf == 1) // C: CMAC-AES
        {
            mac = d_crypto->computeCMAC_AES(applicable_key, m);
        }
    // Truncate the twelve MSBits and compare with received MACSEQ
    uint16_t mac_msb = 0;
    if (!mac.empty())
        {
            mac_msb = (mac[0] << 8) + mac[1];
        }
    uint16_t computed_macseq = (mac_msb & 0xFFF0) >> 4; // TODO - double check, it was 0x0FFF which presuposes little endian...
    int num_tags_added = 0;
    if (computed_macseq == d_osnma_data.d_mack_message.header.macseq)
        {
            std::cout << "OSNMA: MACSEQ authenticated for PRN_A "
                      << osnma_msg->PRN << " with WN="
                      << osnma_msg->WN_sf0 << ", TOW="
                      << osnma_msg->TOW_sf0 << ". Verifying tags. "
                      << std::endl;

            uint8_t l_t_verified = 0; // tag bits verified
            uint8_t i = 0;
            // TODO - configuration file must define which tags shall be verified
            // e.g. NavDataVerification: A == ALL, T == Timing Parameters, ECS == Ephemeris,Clock and Status.
            std::string navDataToVerify = "EphemerisClockAndStatus"; // ADKD 0
            // Timing Parameters ADKD 4
            // EphemerisClockAndStatus ADKD 12 10+ Delay
            m.clear();
            uint8_t lt_bits = 0;
            const auto it2 = OSNMA_TABLE_11.find(d_osnma_data.d_dsm_kroot_message.ts);
            if (it2 != OSNMA_TABLE_11.cend())
                {
                    lt_bits = it2->second;
                }
            if (lt_bits == 0)
                {
                    return; // C: TODO if Tag length is 0, what is the action? no verification possible of NavData for sure.
                }
            while (i < d_osnma_data.d_mack_message.tag_and_info.size() && l_t_verified < d_Lt_min)
                {
                    // compute m
                    m.push_back(d_osnma_data.d_mack_message.tag_and_info[i].tag_info.PRN_d);
                    m.push_back(osnma_msg->PRN);
                    m.push_back(static_cast<uint8_t>((d_GST_Sf & 0xFF000000) >> 24));
                    m.push_back(static_cast<uint8_t>((d_GST_Sf & 0x00FF0000) >> 16));
                    m.push_back(static_cast<uint8_t>((d_GST_Sf & 0x0000FF00) >> 8));
                    m.push_back(static_cast<uint8_t>(d_GST_Sf & 0x000000FF));
                    m.push_back(i+1); // CTRauto
                    m.push_back(d_osnma_data.d_nma_header.nmas);
                    // TODO - other ADKD need different data.
                    // TODO - store buffer the NavData of 11 last subframes, ADKD 0 and 12 => NavData belongs to SF-1
                    m.insert(m.end(),osnma_msg->EphemerisClockAndStatusData.begin(),osnma_msg->EphemerisClockAndStatusData.end()) ;
                    i = 0;
                    while (i<10/*TODO - number of padding zeroes to be computed*/)
                        {
                            m.push_back(0);
                            i++;
                        }

                    // compute mac
                    if (d_osnma_data.d_dsm_kroot_message.mf == 0) // C: HMAC-SHA-256
                        {
                            mac = d_crypto->computeHMAC_SHA_256(applicable_key, m);
                        }
                    else if (d_osnma_data.d_dsm_kroot_message.mf == 1) // C: CMAC-AES
                        {
                            mac = d_crypto->computeCMAC_AES(applicable_key, m);
                        }

                    // compute tag = trunc(l_t, mac(K,m)) Eq. 23 ICD
                    uint64_t computed_mac = static_cast<uint64_t>(mac[0]) << (lt_bits - 8);
                    computed_mac += (static_cast<uint64_t>(mac[1]) << (lt_bits - 16));
                    if (lt_bits == 20)
                        {
                            computed_mac += (static_cast<uint64_t>(mac[1] & 0xF0) >> 4);
                        }
                    else if (lt_bits == 24)
                        {
                            computed_mac += static_cast<uint64_t>(mac[2]);
                        }
                    else if (lt_bits == 28)
                        {
                            computed_mac += (static_cast<uint64_t>(mac[2]) << 4);
                            computed_mac += (static_cast<uint64_t>(mac[3] & 0xF0) >> 4);
                        }
                    else if (lt_bits == 32)
                        {
                            computed_mac += (static_cast<uint64_t>(mac[2]) << 8);
                            computed_mac += static_cast<uint64_t>(mac[3]);
                        }
                    else if (lt_bits == 40)
                        {
                            computed_mac += (static_cast<uint64_t>(mac[2]) << 16);
                            computed_mac += (static_cast<uint64_t>(mac[3]) << 8);
                            computed_mac += static_cast<uint64_t>(mac[4]);
                        }

                    // Compare computed tag with received one truncated
                    if (d_osnma_data.d_mack_message.tag_and_info[i].tag == computed_mac)
                        {
                            std::cout << "Galileo OSNMA: Tag verification successful " << std::endl;
                            l_t_verified += lt_bits;
                        }
                    else
                        {
                            std::cout << "Galileo OSNMA: Tag verification failed " << std::endl;
                        }

                }
        }
}

bool osnma_msg_receiver::verify_dsm_pkr(DSM_PKR_message message)
{
    // TODO create function for recursively apply hash

    // build base leaf m_i
//    auto leaf = message.mid;
    std::vector<uint8_t> m_i;
    m_i.reserve(2 + message.npk.size());
    m_i[0] = message.npkt;
    m_i[1] = message.npktid;
    for (uint8_t i = 2; i < m_i.size(); i++)
        {
            m_i.push_back(message.npk[i]);
        }

    // compute intermediate leafs' values
    std::vector<uint8_t> x_0,x_1,x_2,x_3,x_4;
//    uint8_t  k = 0;
    x_0 = d_crypto->computeSHA256(m_i);
    x_0.insert(x_0.end(),message.itn.begin(),&message.itn[31]);
    x_1 = d_crypto->computeSHA256(x_0);
    x_1.insert(x_1.end(),&message.itn[32],&message.itn[63]);
    x_2 = d_crypto->computeSHA256(x_1);
    x_2.insert(x_2.end(),&message.itn[64],&message.itn[95]);
    x_3 = d_crypto->computeSHA256(x_2);
    x_3.insert(x_3.end(),&message.itn[96],&message.itn[127]);
    // root leaf computation
    x_4 = d_crypto->computeSHA256(x_3);

    // C: d_crypto->getMerkleRoot([m_0:m_15]) I realised I could have done this...
    // C: ... but why computing all the possible results?  I have only one leaf in each osnma message...
    // verify that computed root matches merkle root

    if(x_4 == d_crypto->getMerkleRoot())
        {
            std::cout << "Galileo OSNMA: DSM-PKR verified successfully! " << std::endl;
            return true;
        }
    else
        {
            std::cout << "Galileo OSNMA: DSM-PKR verification unsuccessful !" << std::endl;
            return false;
        }
}
