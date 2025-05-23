/*!
 * \file dll_pll_veml_tracking_fpga.cc
 * \brief Implementation of a code DLL + carrier PLL tracking block using an FPGA
 * \author Marc Majoral, 2019. marc.majoral(at)cttc.es
 * \author Javier Arribas, 2019. jarribas(at)cttc.es
 *
 * Code DLL + carrier PLL according to the algorithms described in:
 * [1] K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency
 * Approach, Birkhauser, 2007
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "dll_pll_veml_tracking_fpga.h"
#include "GPS_L1_CA.h"
#include "GPS_L2C.h"
#include "GPS_L5.h"
#include "Galileo_E1.h"
#include "Galileo_E5a.h"
#include "MATH_CONSTANTS.h"
#include "fpga_multicorrelator.h"
#include "gnss_satellite.h"
#include "gnss_sdr_create_directory.h"
#include "gnss_sdr_filesystem.h"
#include "gnss_synchro.h"
#include "gps_sdr_signal_replica.h"
#include "lock_detectors.h"
#include "tracking_discriminators.h"
#include <gnuradio/io_signature.h>   // for io_signature
#include <gnuradio/thread/thread.h>  // for scoped_lock
#include <matio.h>                   // for Mat_VarCreate
#include <pmt/pmt_sugar.h>           // for mp
#include <volk_gnsssdr/volk_gnsssdr.h>
#include <algorithm>  // for fill_n
#include <cmath>      // for fmod, round, floor
#include <exception>  // for exception
#include <iostream>   // for cout, cerr
#include <map>
#include <numeric>
#include <vector>

#if USE_GLOG_AND_GFLAGS
#include <glog/logging.h>
#else
#include <absl/log/log.h>
#endif

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

dll_pll_veml_tracking_fpga_sptr dll_pll_veml_make_tracking_fpga(const Dll_Pll_Conf_Fpga &conf_)
{
    return dll_pll_veml_tracking_fpga_sptr(new dll_pll_veml_tracking_fpga(conf_));
}


dll_pll_veml_tracking_fpga::dll_pll_veml_tracking_fpga(const Dll_Pll_Conf_Fpga &conf_)
    : gr::block("dll_pll_veml_tracking_fpga", gr::io_signature::make(0, 0, sizeof(lv_16sc_t)),
          gr::io_signature::make(1, 1, sizeof(Gnss_Synchro))),
      d_trk_parameters(conf_),
      d_acquisition_gnss_synchro(nullptr),
      d_code_chip_rate(0.0),
      d_code_phase_step_chips(0.0),
      d_code_phase_rate_step_chips(0.0),
      d_carrier_phase_step_rad(0.0),
      d_carrier_phase_rate_step_rad(0.0),
      d_acq_code_phase_samples(0.0),
      d_acq_carrier_doppler_hz(0.0),
      d_rem_code_phase_samples(0.0),
      d_rem_code_phase_samples_prev(0.0),
      d_current_correlation_time_s(0.0),
      d_carrier_doppler_hz(0.0),
      d_acc_carrier_phase_rad(0.0),
      d_rem_code_phase_chips(0.0),
      d_T_chip_seconds(0.0),
      d_T_prn_seconds(0.0),
      d_T_prn_samples(0.0),
      d_K_blk_samples(0.0),
      d_carrier_lock_test(1.0),
      d_CN0_SNV_dB_Hz(0.0),
      d_carrier_lock_threshold(d_trk_parameters.carrier_lock_th),
      d_sample_counter(0ULL),
      d_acq_sample_stamp(0ULL),
      d_sample_counter_next(0ULL),
      d_rem_carr_phase_rad(0.0),
      d_state(1),
      d_extend_correlation_symbols_count(0),
      d_current_integration_length_samples(static_cast<int32_t>(d_trk_parameters.vector_length)),
      d_cn0_estimation_counter(0),
      d_carrier_lock_fail_counter(0),
      d_code_lock_fail_counter(0),
      d_extend_fpga_integration_periods(d_trk_parameters.extend_fpga_integration_periods),
      d_channel(0),
      d_secondary_code_length(0U),
      d_data_secondary_code_length(0U),
      d_code_length_chips(d_trk_parameters.code_length_chips),
      d_code_samples_per_chip(d_trk_parameters.code_samples_per_chip),
      d_fpga_integration_period(d_trk_parameters.fpga_integration_period),
      d_current_fpga_integration_period(1),
      d_veml(false),
      d_cloop(true),
      d_dump(d_trk_parameters.dump),
      d_dump_mat(d_trk_parameters.dump_mat && d_dump),
      d_pull_in_transitory(true),
      d_corrected_doppler(false),
      d_interchange_iq(false),
      d_acc_carrier_phase_initialized(false),
      d_worker_is_done(false),
      d_extended_correlation_in_fpga(d_trk_parameters.extended_correlation_in_fpga),
      d_current_extended_correlation_in_fpga(false),
      d_stop_tracking(false),
      d_sc_demodulate_enabled(false),
      d_Flag_PLL_180_deg_phase_locked(false)
{
#if GNURADIO_GREATER_THAN_38
    this->set_relative_rate(1, static_cast<uint64_t>(d_trk_parameters.vector_length));
#else
    this->set_relative_rate(1.0 / static_cast<double>(d_trk_parameters.vector_length));
#endif
    // prevent telemetry symbols accumulation in output buffers
    this->set_max_noutput_items(1);

    // Telemetry bit synchronization message port input
    this->message_port_register_out(pmt::mp("events"));

    // Telemetry message port input
    this->message_port_register_in(pmt::mp("telemetry_to_trk"));
    this->set_msg_handler(pmt::mp("telemetry_to_trk"),
#if HAS_GENERIC_LAMBDA
        [this](auto &&PH1) { msg_handler_telemetry_to_trk(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&dll_pll_veml_tracking_fpga::msg_handler_telemetry_to_trk, this, boost::placeholders::_1));
#else
        boost::bind(&dll_pll_veml_tracking_fpga::msg_handler_telemetry_to_trk, this, _1));
#endif
#endif
    // initialize internal vars
    d_dll_filt_history.set_capacity(1000);

    d_signal_type = std::string(d_trk_parameters.signal);

    std::map<std::string, std::string> map_signal_pretty_name;
    map_signal_pretty_name["1C"] = "L1 C/A";
    map_signal_pretty_name["1B"] = "E1";
    map_signal_pretty_name["1G"] = "L1 C/A";
    map_signal_pretty_name["2S"] = "L2C";
    map_signal_pretty_name["2G"] = "L2 C/A";
    map_signal_pretty_name["5X"] = "E5a";
    map_signal_pretty_name["L5"] = "L5";

    d_signal_pretty_name = map_signal_pretty_name[d_signal_type];

    if (d_trk_parameters.system == 'G')
        {
            d_systemName = "GPS";
            if (d_signal_type == "1C")
                {
                    d_signal_carrier_freq = GPS_L1_FREQ_HZ;
                    d_code_period = GPS_L1_CA_CODE_PERIOD_S;
                    d_code_chip_rate = GPS_L1_CA_CODE_RATE_CPS;
                    d_correlation_length_ms = 1;
                    // GPS L1 C/A does not have pilot component nor secondary code
                    d_secondary = false;
                    d_trk_parameters.track_pilot = false;
                    d_trk_parameters.slope = 1.0;
                    d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                    d_trk_parameters.y_intercept = 1.0;
                    // symbol integration: 20 trk symbols (20 ms) = 1 tlm bit
                    // set the bit transition pattern in secondary code to obtain bit synchronization
                    d_secondary_code_length = static_cast<uint32_t>(GPS_CA_PREAMBLE_LENGTH_SYMBOLS);
                    d_secondary_code_string = GPS_CA_PREAMBLE_SYMBOLS_STR;
                    d_symbols_per_bit = GPS_CA_TELEMETRY_SYMBOLS_PER_BIT;
                }
            else if (d_signal_type == "2S")
                {
                    d_signal_carrier_freq = GPS_L2_FREQ_HZ;
                    d_code_period = GPS_L2_M_PERIOD_S;
                    d_code_chip_rate = GPS_L2_M_CODE_RATE_CPS;
                    // GPS L2C has 1 trk symbol (20 ms) per tlm bit, no symbol integration required
                    d_symbols_per_bit = GPS_L2_SAMPLES_PER_SYMBOL;
                    d_correlation_length_ms = 20;
                    d_trk_parameters.slope = 1.0;
                    d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                    d_trk_parameters.y_intercept = 1.0;
                    // GPS L2 does not have pilot component nor secondary code
                    d_secondary = false;
                    d_trk_parameters.track_pilot = false;
                }
            else if (d_signal_type == "L5")
                {
                    d_signal_carrier_freq = GPS_L5_FREQ_HZ;
                    d_code_period = GPS_L5I_PERIOD_S;
                    d_code_chip_rate = GPS_L5I_CODE_RATE_CPS;
                    // symbol integration: 10 trk symbols (10 ms) = 1 tlm bit
                    d_symbols_per_bit = GPS_L5_SAMPLES_PER_SYMBOL;
                    d_correlation_length_ms = 1;
                    d_secondary = true;
                    d_trk_parameters.slope = 1.0;
                    d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                    d_trk_parameters.y_intercept = 1.0;
                    if (d_extended_correlation_in_fpga == true)
                        {
                            if (d_trk_parameters.extend_correlation_symbols > 1)
                                {
                                    d_sc_demodulate_enabled = true;
                                }
                        }
                    if (d_trk_parameters.track_pilot)
                        {
                            // synchronize pilot secondary code
                            d_secondary_code_length = static_cast<uint32_t>(GPS_L5Q_NH_CODE_LENGTH);
                            d_secondary_code_string = GPS_L5Q_NH_CODE_STR;
                            // remove data secondary code
                            // remove Neuman-Hofman Code (see IS-GPS-705D)
                            d_data_secondary_code_length = static_cast<uint32_t>(GPS_L5I_NH_CODE_LENGTH);
                            d_data_secondary_code_string = GPS_L5I_NH_CODE_STR;
                            d_signal_pretty_name = d_signal_pretty_name + "Q";
                        }
                    else
                        {
                            // synchronize and remove data secondary code
                            // remove Neuman-Hofman Code (see IS-GPS-705D)
                            d_secondary_code_length = static_cast<uint32_t>(GPS_L5I_NH_CODE_LENGTH);
                            d_secondary_code_string = GPS_L5I_NH_CODE_STR;
                            d_signal_pretty_name = d_signal_pretty_name + "I";
                            d_interchange_iq = true;
                        }
                }
            else
                {
                    LOG(WARNING) << "Invalid Signal argument when instantiating tracking blocks";
                    std::cerr << "Invalid Signal argument when instantiating tracking blocks\n";
                    d_correlation_length_ms = 1;
                    d_secondary = false;
                    d_signal_carrier_freq = 0.0;
                    d_code_period = 0.0;
                    d_symbols_per_bit = 0;
                }
        }
    else if (d_trk_parameters.system == 'E')
        {
            d_systemName = "Galileo";
            if (d_signal_type == "1B")
                {
                    d_signal_carrier_freq = GALILEO_E1_FREQ_HZ;
                    d_code_period = GALILEO_E1_CODE_PERIOD_S;
                    d_code_chip_rate = GALILEO_E1_CODE_CHIP_RATE_CPS;
                    // Galileo E1b has 1 trk symbol (4 ms) per tlm bit, no symbol integration required
                    d_symbols_per_bit = 1;
                    d_correlation_length_ms = 4;
                    d_veml = true;
                    d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                    d_trk_parameters.slope = static_cast<float>(-CalculateSlopeAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc));
                    d_trk_parameters.y_intercept = static_cast<float>(GetYInterceptAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc));
                    if (d_trk_parameters.track_pilot)
                        {
                            d_secondary = true;
                            d_secondary_code_length = static_cast<uint32_t>(GALILEO_E1_C_SECONDARY_CODE_LENGTH);
                            d_secondary_code_string = GALILEO_E1_C_SECONDARY_CODE;
                            d_signal_pretty_name = d_signal_pretty_name + "C";
                        }
                    else
                        {
                            d_secondary = false;
                            d_signal_pretty_name = d_signal_pretty_name + "B";
                        }
                    // Note that E1-B and E1-C are in anti-phase, NOT IN QUADRATURE. See Galileo ICD.
                }
            else if (d_signal_type == "5X")
                {
                    d_signal_carrier_freq = GALILEO_E5A_FREQ_HZ;
                    d_code_period = GALILEO_E5A_CODE_PERIOD_S;
                    d_code_chip_rate = GALILEO_E5A_CODE_CHIP_RATE_CPS;
                    d_symbols_per_bit = 20;
                    d_correlation_length_ms = 1;
                    d_secondary = true;
                    d_trk_parameters.slope = 1.0;
                    d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                    d_trk_parameters.y_intercept = 1.0;
                    if (d_extended_correlation_in_fpga == true)
                        {
                            if (d_trk_parameters.extend_correlation_symbols > 1)
                                {
                                    d_sc_demodulate_enabled = true;
                                }
                        }
                    if (d_trk_parameters.track_pilot)
                        {
                            // synchronize pilot secondary code
                            d_secondary_code_length = static_cast<uint32_t>(GALILEO_E5A_Q_SECONDARY_CODE_LENGTH);
                            d_signal_pretty_name = d_signal_pretty_name + "Q";
                            // remove data secondary code
                            d_data_secondary_code_length = static_cast<uint32_t>(GALILEO_E5A_I_SECONDARY_CODE_LENGTH);
                            d_data_secondary_code_string = GALILEO_E5A_I_SECONDARY_CODE;
                            d_interchange_iq = true;
                            // the pilot secondary code depends on PRN and it is initialized later
                        }
                    else
                        {
                            // synchronize and remove data secondary code
                            d_secondary_code_length = static_cast<uint32_t>(GALILEO_E5A_I_SECONDARY_CODE_LENGTH);
                            d_secondary_code_string = GALILEO_E5A_I_SECONDARY_CODE;
                            d_signal_pretty_name = d_signal_pretty_name + "I";
                        }
                }
            else
                {
                    LOG(WARNING) << "Invalid Signal argument when instantiating tracking blocks";
                    std::cout << "Invalid Signal argument when instantiating tracking blocks\n";
                    d_correlation_length_ms = 1;
                    d_secondary = false;
                    d_signal_carrier_freq = 0.0;
                    d_code_period = 0.0;
                    d_symbols_per_bit = 0;
                }
        }
    else
        {
            LOG(WARNING) << "Invalid System argument when instantiating tracking blocks";
            std::cerr << "Invalid System argument when instantiating tracking blocks\n";
            d_correlation_length_ms = 1;
            d_secondary = false;
            d_signal_carrier_freq = 0.0;
            d_code_period = 0.0;
            d_symbols_per_bit = 0;
        }

    // Initialize tracking  ==========================================
    d_code_loop_filter = Tracking_loop_filter(static_cast<float>(d_code_period), d_trk_parameters.dll_bw_hz, d_trk_parameters.dll_filter_order, false);
    d_carrier_loop_filter.set_params(d_trk_parameters.fll_bw_hz, d_trk_parameters.pll_bw_hz, d_trk_parameters.pll_filter_order);

    // correlator outputs (scalar)
    if (d_veml)
        {
            // Very-Early, Early, Prompt, Late, Very-Late
            d_n_correlator_taps = 5;
        }
    else
        {
            // Early, Prompt, Late
            d_n_correlator_taps = 3;
        }

    d_correlator_outs = volk_gnsssdr::vector<gr_complex>(d_n_correlator_taps);
    d_local_code_shift_chips = volk_gnsssdr::vector<float>(d_n_correlator_taps);
    // map memory pointers of correlator outputs
    if (d_veml)
        {
            d_Very_Early = &d_correlator_outs[0];
            d_Early = &d_correlator_outs[1];
            d_Prompt = &d_correlator_outs[2];
            d_Late = &d_correlator_outs[3];
            d_Very_Late = &d_correlator_outs[4];
            d_local_code_shift_chips[0] = -d_trk_parameters.very_early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_local_code_shift_chips[1] = -d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_local_code_shift_chips[2] = 0.0;
            d_local_code_shift_chips[3] = d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_local_code_shift_chips[4] = d_trk_parameters.very_early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_prompt_data_shift = &d_local_code_shift_chips[2];
        }
    else
        {
            d_Very_Early = nullptr;
            d_Early = &d_correlator_outs[0];
            d_Prompt = &d_correlator_outs[1];
            d_Late = &d_correlator_outs[2];
            d_Very_Late = nullptr;
            d_local_code_shift_chips[0] = -d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_local_code_shift_chips[1] = 0.0;
            d_local_code_shift_chips[2] = d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
            d_prompt_data_shift = &d_local_code_shift_chips[1];
        }

    if (d_trk_parameters.extend_correlation_symbols > 1)
        {
            d_enable_extended_integration = true;
        }
    else
        {
            d_enable_extended_integration = false;
            d_trk_parameters.extend_correlation_symbols = 1;
        }

    // --- Initializations ---
    d_Prompt_circular_buffer.set_capacity(d_secondary_code_length);

    // Initial code frequency basis of NCO
    d_code_freq_chips = d_code_chip_rate;
    d_next_integration_length_samples = d_current_integration_length_samples;

    // CN0 estimation and lock detector buffers
    d_Prompt_buffer = volk_gnsssdr::vector<gr_complex>(d_trk_parameters.cn0_samples);
    d_Prompt_Data = volk_gnsssdr::vector<gr_complex>(1);
    d_cn0_smoother = Exponential_Smoother();
    d_cn0_smoother.set_alpha(d_trk_parameters.cn0_smoother_alpha);
    if (d_code_period > 0.0)
        {
            d_cn0_smoother.set_samples_for_initialization(d_trk_parameters.cn0_smoother_samples / static_cast<int>(d_code_period * 1000.0));
        }

    d_carrier_lock_test_smoother = Exponential_Smoother();
    d_carrier_lock_test_smoother.set_alpha(d_trk_parameters.carrier_lock_test_smoother_alpha);
    d_carrier_lock_test_smoother.set_min_value(-1.0);
    d_carrier_lock_test_smoother.set_offset(0.0);
    d_carrier_lock_test_smoother.set_samples_for_initialization(d_trk_parameters.carrier_lock_test_smoother_samples);

    clear_tracking_vars();
    if (d_trk_parameters.smoother_length > 0)
        {
            d_carr_ph_history.set_capacity(d_trk_parameters.smoother_length * 2);
            d_code_ph_history.set_capacity(d_trk_parameters.smoother_length * 2);
        }
    else
        {
            d_carr_ph_history.set_capacity(1);
            d_code_ph_history.set_capacity(1);
        }

    // create multicorrelator class
    int32_t *ca_codes = d_trk_parameters.ca_codes;
    int32_t *data_codes = d_trk_parameters.data_codes;
    d_multicorrelator_fpga = std::make_shared<Fpga_Multicorrelator_8sc>(d_n_correlator_taps, ca_codes, data_codes, d_code_length_chips, d_trk_parameters.track_pilot, d_code_samples_per_chip);
    d_multicorrelator_fpga->set_output_vectors(d_correlator_outs.data(), d_Prompt_Data.data());

    if (d_dump)
        {
            d_dump_filename = d_trk_parameters.dump_filename;
            std::string dump_path;
            // Get path
            if (d_dump_filename.find_last_of('/') != std::string::npos)
                {
                    std::string dump_filename_ = d_dump_filename.substr(d_dump_filename.find_last_of('/') + 1);
                    dump_path = d_dump_filename.substr(0, d_dump_filename.find_last_of('/'));
                    d_dump_filename = std::move(dump_filename_);
                }
            else
                {
                    dump_path = std::string(".");
                }
            if (d_dump_filename.empty())
                {
                    d_dump_filename = "trk_channel_";
                }
            // remove extension if any
            if (d_dump_filename.substr(1).find_last_of('.') != std::string::npos)
                {
                    d_dump_filename = d_dump_filename.substr(0, d_dump_filename.find_last_of('.'));
                }

            d_dump_filename = dump_path + fs::path::preferred_separator + d_dump_filename;
            // create directory
            if (!gnss_sdr_create_directory(dump_path))
                {
                    std::cerr << "GNSS-SDR cannot create dump files for the tracking block. Wrong permissions?\n";
                    d_dump = false;
                }
        }
}


void dll_pll_veml_tracking_fpga::msg_handler_telemetry_to_trk(const pmt::pmt_t &msg)
{
    try
        {
            if (pmt::any_ref(msg).type().hash_code() == int_type_hash_code)
                {
                    const int tlm_event = wht::any_cast<int>(pmt::any_ref(msg));
                    if (tlm_event == 1)
                        {
                            DLOG(INFO) << "Telemetry fault received in ch " << this->d_channel;
                            gr::thread::scoped_lock lock(d_setlock);
                            d_carrier_lock_fail_counter = 200000;  // force loss-of-lock condition
                        }
                }
        }
    catch (const wht::bad_any_cast &e)
        {
            LOG(WARNING) << "msg_handler_telemetry_to_trk Bad any_cast: " << e.what();
        }
}


void dll_pll_veml_tracking_fpga::start_tracking()
{
    // all the calculations that do not require the data from the acquisition module are moved to the
    // set_gnss_synchro command, which is received with a valid PRN before the acquisition module starts the
    // acquisition process. This is done to minimize the time between the end of the acquisition process and
    // the beginning of the tracking process.

    //  correct the code phase according to the delay between acq and trk
    d_acq_code_phase_samples = d_acquisition_gnss_synchro->Acq_delay_samples;
    d_acq_carrier_doppler_hz = d_acquisition_gnss_synchro->Acq_doppler_hz;
    d_acq_sample_stamp = d_acquisition_gnss_synchro->Acq_samplestamp_samples;

    d_carrier_doppler_hz = d_acq_carrier_doppler_hz;
    d_carrier_phase_step_rad = TWO_PI * d_carrier_doppler_hz / d_trk_parameters.fs_in;

    // filter initialization
    d_carrier_loop_filter.initialize(static_cast<float>(d_acq_carrier_doppler_hz));  // initialize the carrier filter

    d_corrected_doppler = false;
    d_acc_carrier_phase_initialized = false;

    boost::mutex::scoped_lock lock(d_mutex);
    d_worker_is_done = true;
    d_m_condition.notify_one();
}


dll_pll_veml_tracking_fpga::~dll_pll_veml_tracking_fpga()
{
    if (d_dump_file.is_open())
        {
            try
                {
                    d_dump_file.close();
                }
            catch (const std::exception &ex)
                {
                    LOG(WARNING) << "Exception in Tracking block destructor: " << ex.what();
                }
        }
    if (d_dump_mat)
        {
            try
                {
                    save_matfile();
                }
            catch (const std::exception &ex)
                {
                    LOG(WARNING) << "Error saving the .mat file: " << ex.what();
                }
        }
    try
        {
            d_multicorrelator_fpga->free();
        }
    catch (const std::exception &ex)
        {
            LOG(WARNING) << "Exception in Tracking block destructor: " << ex.what();
        }
}


bool dll_pll_veml_tracking_fpga::acquire_secondary()
{
    // ******* preamble correlation ********
    int32_t corr_value = 0;
    for (uint32_t i = 0; i < d_secondary_code_length; i++)
        {
            if (d_Prompt_circular_buffer[i].real() < 0.0)  // symbols clipping
                {
                    if (d_secondary_code_string[i] == '0')
                        {
                            corr_value++;
                        }
                    else
                        {
                            corr_value--;
                        }
                }
            else
                {
                    if (d_secondary_code_string[i] == '0')
                        {
                            corr_value--;
                        }
                    else
                        {
                            corr_value++;
                        }
                }
        }

    if (abs(corr_value) == static_cast<int32_t>(d_secondary_code_length))
        {
            if (corr_value < 0)
                {
                    d_Flag_PLL_180_deg_phase_locked = true;
                }
            else
                {
                    d_Flag_PLL_180_deg_phase_locked = false;
                }
            return true;
        }

    return false;
}


bool dll_pll_veml_tracking_fpga::cn0_and_tracking_lock_status(double coh_integration_time_s)
{
    // ####### CN0 ESTIMATION AND LOCK DETECTORS ######
    if (d_cn0_estimation_counter < d_trk_parameters.cn0_samples)
        {
            // fill buffer with prompt correlator output values
            d_Prompt_buffer[d_cn0_estimation_counter] = d_P_accu;
            d_cn0_estimation_counter++;
            return true;
        }
    d_Prompt_buffer[d_cn0_estimation_counter % d_trk_parameters.cn0_samples] = d_P_accu;
    d_cn0_estimation_counter++;
    // Code lock indicator
    const float d_CN0_SNV_dB_Hz_raw = cn0_m2m4_estimator(d_Prompt_buffer.data(), d_trk_parameters.cn0_samples, static_cast<float>(coh_integration_time_s));
    d_CN0_SNV_dB_Hz = d_cn0_smoother.smooth(d_CN0_SNV_dB_Hz_raw);
    // Carrier lock indicator
    d_carrier_lock_test = d_carrier_lock_test_smoother.smooth(carrier_lock_detector(d_Prompt_buffer.data(), 1));
    // Loss of lock detection
    if (!d_pull_in_transitory)
        {
            if (d_carrier_lock_test < d_carrier_lock_threshold)
                {
                    d_carrier_lock_fail_counter++;
                }
            else
                {
                    if (d_carrier_lock_fail_counter > 0)
                        {
                            d_carrier_lock_fail_counter--;
                        }
                }

            if (d_CN0_SNV_dB_Hz < d_trk_parameters.cn0_min)
                {
                    d_code_lock_fail_counter++;
                }
            else
                {
                    if (d_code_lock_fail_counter > 0)
                        {
                            d_code_lock_fail_counter--;
                        }
                }
        }
    if (d_carrier_lock_fail_counter > d_trk_parameters.max_carrier_lock_fail or d_code_lock_fail_counter > d_trk_parameters.max_code_lock_fail)
        {
            std::cout << "Loss of lock in channel " << d_channel << "!\n";
            LOG(INFO) << "Loss of lock in channel " << d_channel
                      << " (carrier_lock_fail_counter:" << d_carrier_lock_fail_counter
                      << " code_lock_fail_counter : " << d_code_lock_fail_counter << ")";
            this->message_port_pub(pmt::mp("events"), pmt::from_long(3));  // 3 -> loss of lock
            d_carrier_lock_fail_counter = 0;
            d_code_lock_fail_counter = 0;
            d_multicorrelator_fpga->unlock_channel();
            return false;
        }
    return true;
}


// correlation requires:
// - updated remnant carrier phase in radians (rem_carr_phase_rad)
// - updated remnant code phase in samples (d_rem_code_phase_samples)
// - d_code_freq_chips
// - d_carrier_doppler_hz
void dll_pll_veml_tracking_fpga::do_correlation_step()
{
    // ################# CARRIER WIPEOFF AND CORRELATORS ##############################
    // perform carrier wipe-off and compute Early, Prompt and Late correlation

    d_multicorrelator_fpga->Carrier_wipeoff_multicorrelator_resampler(
        d_rem_carr_phase_rad,
        static_cast<float>(d_carrier_phase_step_rad), static_cast<float>(d_carrier_phase_rate_step_rad),
        static_cast<float>(d_rem_code_phase_chips) * static_cast<float>(d_code_samples_per_chip),
        static_cast<float>(d_code_phase_step_chips) * static_cast<float>(d_code_samples_per_chip),
        static_cast<float>(d_code_phase_rate_step_chips) * static_cast<float>(d_code_samples_per_chip),
        d_current_integration_length_samples);
}


void dll_pll_veml_tracking_fpga::run_dll_pll()
{
    // ################## PLL ##########################################################
    // PLL discriminator
    if (d_cloop)
        {
            // Costas loop discriminator, insensitive to 180 deg phase transitions
            d_carr_phase_error_hz = pll_cloop_two_quadrant_atan(d_P_accu) / TWO_PI;
        }
    else
        {
            // Secondary code acquired. No symbols transition should be present in the signal
            d_carr_phase_error_hz = pll_four_quadrant_atan(d_P_accu) / TWO_PI;
        }

    if ((d_pull_in_transitory == true and d_trk_parameters.enable_fll_pull_in == true) or d_trk_parameters.enable_fll_steady_state)
        {
            // FLL discriminator
            // d_carr_freq_error_hz = fll_four_quadrant_atan(d_P_accu_old, d_P_accu, 0, d_current_correlation_time_s) / TWO_PI;
            d_carr_freq_error_hz = fll_diff_atan(d_P_accu_old, d_P_accu, 0, d_current_correlation_time_s) / TWO_PI;

            d_P_accu_old = d_P_accu;
            // std::cout << "d_carr_freq_error_hz: " << d_carr_freq_error_hz << '\n';
            // Carrier discriminator filter
            if ((d_pull_in_transitory == true and d_trk_parameters.enable_fll_pull_in == true))
                {
                    // pure FLL, disable PLL
                    d_carr_error_filt_hz = d_carrier_loop_filter.get_carrier_error(static_cast<float>(d_carr_freq_error_hz), 0, static_cast<float>(d_current_correlation_time_s));
                }
            else
                {
                    // FLL-aided PLL
                    d_carr_error_filt_hz = d_carrier_loop_filter.get_carrier_error(static_cast<float>(d_carr_freq_error_hz), static_cast<float>(d_carr_phase_error_hz), static_cast<float>(d_current_correlation_time_s));
                }
        }
    else
        {
            // Carrier discriminator filter
            d_carr_error_filt_hz = d_carrier_loop_filter.get_carrier_error(0, static_cast<float>(d_carr_phase_error_hz), static_cast<float>(d_current_correlation_time_s));
        }

    // New carrier Doppler frequency estimation
    d_carrier_doppler_hz = d_carr_error_filt_hz;

    //    std::cout << "d_carrier_doppler_hz: " << d_carrier_doppler_hz << '\n';
    //    std::cout << "d_CN0_SNV_dB_Hz: " << this->d_CN0_SNV_dB_Hz << '\n';

    // ################## DLL ##########################################################
    // DLL discriminator
    if (d_veml)
        {
            d_code_error_chips = dll_nc_vemlp_normalized(d_VE_accu, d_E_accu, d_L_accu, d_VL_accu);  // [chips/Ti]
        }
    else
        {
            d_code_error_chips = dll_nc_e_minus_l_normalized(d_E_accu, d_L_accu, d_trk_parameters.spc, d_trk_parameters.slope, d_trk_parameters.y_intercept);  // [chips/Ti]
        }
    // Code discriminator filter
    d_code_error_filt_chips = d_code_loop_filter.apply(static_cast<float>(d_code_error_chips));  // [chips/second]
    // New code Doppler frequency estimation
    d_code_freq_chips = d_code_chip_rate - d_code_error_filt_chips;
    if (d_trk_parameters.carrier_aiding)
        {
            d_code_freq_chips += d_carrier_doppler_hz * d_code_chip_rate / d_signal_carrier_freq;
        }

    // Experimental: detect Carrier Doppler vs. Code Doppler incoherence and correct the Carrier Doppler
    if (d_trk_parameters.enable_doppler_correction == true)
        {
            if (d_pull_in_transitory == false and d_corrected_doppler == false)
                {
                    d_dll_filt_history.push_back(static_cast<float>(d_code_error_filt_chips));

                    if (d_dll_filt_history.full())
                        {
                            const float avg_code_error_chips_s = static_cast<float>(std::accumulate(d_dll_filt_history.begin(), d_dll_filt_history.end(), 0.0)) / static_cast<float>(d_dll_filt_history.capacity());
                            if (std::fabs(avg_code_error_chips_s) > 1.0)
                                {
                                    const float carrier_doppler_error_hz = static_cast<float>(d_signal_carrier_freq) * avg_code_error_chips_s / static_cast<float>(d_code_chip_rate);
                                    LOG(INFO) << "Detected and corrected carrier doppler error: " << carrier_doppler_error_hz << " [Hz] on sat " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN);
                                    d_carrier_loop_filter.initialize(static_cast<float>(d_carrier_doppler_hz) - carrier_doppler_error_hz);
                                    d_corrected_doppler = true;
                                }
                            d_dll_filt_history.clear();
                        }
                }
        }
}


void dll_pll_veml_tracking_fpga::check_carrier_phase_coherent_initialization()
{
    if (d_acc_carrier_phase_initialized == false)
        {
            d_acc_carrier_phase_rad = -d_rem_carr_phase_rad;
            d_acc_carrier_phase_initialized = true;
        }
}


void dll_pll_veml_tracking_fpga::clear_tracking_vars()
{
    std::fill_n(d_correlator_outs.begin(), d_n_correlator_taps, gr_complex(0.0, 0.0));
    if (d_trk_parameters.track_pilot)
        {
            d_Prompt_Data[0] = gr_complex(0.0, 0.0);
            d_P_data_accu = gr_complex(0.0, 0.0);
        }
    d_P_accu_old = gr_complex(0.0, 0.0);
    d_carr_phase_error_hz = 0.0;
    d_carr_freq_error_hz = 0.0;
    d_carr_error_filt_hz = 0.0;
    d_code_error_chips = 0.0;
    d_code_error_filt_chips = 0.0;
    d_current_symbol = 0;
    d_current_data_symbol = 0;
    d_Prompt_circular_buffer.clear();
    d_carrier_phase_rate_step_rad = 0.0;
    d_code_phase_rate_step_chips = 0.0;
    d_carr_ph_history.clear();
    d_code_ph_history.clear();
}


void dll_pll_veml_tracking_fpga::update_tracking_vars()
{
    d_T_chip_seconds = 1.0 / d_code_freq_chips;
    d_T_prn_seconds = d_T_chip_seconds * static_cast<double>(d_code_length_chips);

    // ################## CARRIER AND CODE NCO BUFFER ALIGNMENT #######################
    // keep alignment parameters for the next input buffer
    // Compute the next buffer length based in the new period of the PRN sequence and the code phase error estimation
    d_T_prn_samples = d_T_prn_seconds * d_trk_parameters.fs_in;
    d_K_blk_samples = d_T_prn_samples * d_current_fpga_integration_period + d_rem_code_phase_samples;  // initially d_rem_code_phase_samples is zero. It is updated at the end of this function
    const auto actual_blk_length = static_cast<int32_t>(std::floor(d_K_blk_samples));
    d_next_integration_length_samples = actual_blk_length;

    // ################## PLL COMMANDS #################################################
    // carrier phase step (NCO phase increment per sample) [rads/sample]
    d_carrier_phase_step_rad = TWO_PI * d_carrier_doppler_hz / d_trk_parameters.fs_in;
    // carrier phase rate step (NCO phase increment rate per sample) [rads/sample^2]
    if (d_trk_parameters.high_dyn)
        {
            d_carr_ph_history.push_back(std::pair<double, double>(d_carrier_phase_step_rad, static_cast<double>(d_current_integration_length_samples)));
            if (d_carr_ph_history.full())
                {
                    double tmp_cp1 = 0.0;
                    double tmp_cp2 = 0.0;
                    double tmp_samples = 0.0;
                    for (unsigned int k = 0; k < d_trk_parameters.smoother_length; k++)
                        {
                            tmp_cp1 += d_carr_ph_history[k].first;
                            tmp_cp2 += d_carr_ph_history[d_trk_parameters.smoother_length * 2 - k - 1].first;
                            tmp_samples += d_carr_ph_history[d_trk_parameters.smoother_length * 2 - k - 1].second;
                        }
                    tmp_cp1 /= static_cast<double>(d_trk_parameters.smoother_length);
                    tmp_cp2 /= static_cast<double>(d_trk_parameters.smoother_length);
                    d_carrier_phase_rate_step_rad = (tmp_cp2 - tmp_cp1) / tmp_samples;
                }
        }
    // std::cout << d_carrier_phase_rate_step_rad * d_trk_parameters.fs_in * d_trk_parameters.fs_in / TWO_PI << '\n';
    // remnant carrier phase to prevent overflow in the code NCO
    d_rem_carr_phase_rad += static_cast<float>(d_carrier_phase_step_rad * static_cast<double>(d_current_integration_length_samples) + 0.5 * d_carrier_phase_rate_step_rad * static_cast<double>(d_current_integration_length_samples) * static_cast<double>(d_current_integration_length_samples));
    d_rem_carr_phase_rad = fmod(d_rem_carr_phase_rad, TWO_PI);

    // carrier phase accumulator
    // double a = d_carrier_phase_step_rad * static_cast<double>(d_current_prn_length_samples);
    // double b = 0.5 * d_carrier_phase_rate_step_rad * static_cast<double>(d_current_prn_length_samples) * static_cast<double>(d_current_prn_length_samples);
    // std::cout << fmod(b, TWO_PI) / fmod(a, TWO_PI) << '\n';
    d_acc_carrier_phase_rad -= (d_carrier_phase_step_rad * static_cast<double>(d_current_integration_length_samples) + 0.5 * d_carrier_phase_rate_step_rad * static_cast<double>(d_current_integration_length_samples) * static_cast<double>(d_current_integration_length_samples));

    // ################## DLL COMMANDS #################################################
    // code phase step (Code resampler phase increment per sample) [chips/sample]
    d_code_phase_step_chips = d_code_freq_chips / d_trk_parameters.fs_in;
    if (d_trk_parameters.high_dyn)
        {
            d_code_ph_history.push_back(std::pair<double, double>(d_code_phase_step_chips, static_cast<double>(d_current_integration_length_samples)));
            if (d_code_ph_history.full())
                {
                    double tmp_cp1 = 0.0;
                    double tmp_cp2 = 0.0;
                    double tmp_samples = 0.0;
                    for (unsigned int k = 0; k < d_trk_parameters.smoother_length; k++)
                        {
                            tmp_cp1 += d_code_ph_history[k].first;
                            tmp_cp2 += d_code_ph_history[d_trk_parameters.smoother_length * 2 - k - 1].first;
                            tmp_samples += d_code_ph_history[d_trk_parameters.smoother_length * 2 - k - 1].second;
                        }
                    tmp_cp1 /= static_cast<double>(d_trk_parameters.smoother_length);
                    tmp_cp2 /= static_cast<double>(d_trk_parameters.smoother_length);
                    if (tmp_samples >= 1.0)
                        {
                            d_code_phase_rate_step_chips = (tmp_cp2 - tmp_cp1) / tmp_samples;
                        }
                }
        }
    // remnant code phase [chips]
    d_rem_code_phase_samples_prev = d_rem_code_phase_samples;
    d_rem_code_phase_samples = d_K_blk_samples - static_cast<double>(d_current_integration_length_samples);  // rounding error < 1 sample
    d_rem_code_phase_chips = d_code_freq_chips * d_rem_code_phase_samples / d_trk_parameters.fs_in;
}


void dll_pll_veml_tracking_fpga::save_correlation_results()
{
    if (d_secondary && (!d_current_extended_correlation_in_fpga))  // the FPGA removes the secondary code
        {
            if (d_secondary_code_string[d_current_symbol] == '0')
                {
                    if (d_veml)
                        {
                            d_VE_accu += *d_Very_Early;
                            d_VL_accu += *d_Very_Late;
                        }
                    d_E_accu += *d_Early;
                    d_P_accu += *d_Prompt;
                    d_L_accu += *d_Late;
                }
            else
                {
                    if (d_veml)
                        {
                            d_VE_accu -= *d_Very_Early;
                            d_VL_accu -= *d_Very_Late;
                        }
                    d_E_accu -= *d_Early;
                    d_P_accu -= *d_Prompt;
                    d_L_accu -= *d_Late;
                }
            d_current_symbol++;
            // secondary code roll-up
            d_current_symbol %= d_secondary_code_length;
        }
    else
        {
            if (d_veml)
                {
                    d_VE_accu += *d_Very_Early;
                    d_VL_accu += *d_Very_Late;
                }
            d_E_accu += *d_Early;
            d_P_accu += *d_Prompt;
            d_L_accu += *d_Late;
        }

    // data secondary code roll-up
    if (d_symbols_per_bit > 1)
        {
            if (d_data_secondary_code_length > 0)
                {
                    if (d_trk_parameters.track_pilot)
                        {
                            if (!d_current_extended_correlation_in_fpga)  // the FPGA removes the secondary code
                                {
                                    if (d_data_secondary_code_string[d_current_data_symbol] == '0')
                                        {
                                            d_P_data_accu += d_Prompt_Data[0];
                                        }
                                    else
                                        {
                                            d_P_data_accu -= d_Prompt_Data[0];
                                        }
                                }
                            else
                                {
                                    d_P_data_accu += d_Prompt_Data[0];
                                }
                        }
                    else
                        {
                            if (!d_current_extended_correlation_in_fpga)
                                {
                                    if (d_data_secondary_code_string[d_current_data_symbol] == '0')
                                        {
                                            d_P_data_accu += *d_Prompt;
                                        }
                                    else
                                        {
                                            d_P_data_accu -= *d_Prompt;
                                        }
                                }
                            else
                                {
                                    d_P_data_accu += *d_Prompt;
                                }
                        }

                    d_current_data_symbol += d_current_fpga_integration_period;
                    d_current_data_symbol %= d_data_secondary_code_length;
                }
            else
                {
                    if (d_trk_parameters.track_pilot)
                        {
                            d_P_data_accu += d_Prompt_Data[0];
                        }
                    else
                        {
                            d_P_data_accu += *d_Prompt;
                            // std::cout << "s[" << d_current_data_symbol << "]=" << (int)((*d_Prompt).real() > 0) << '\n';
                        }
                    d_current_data_symbol += d_current_fpga_integration_period;
                    d_current_data_symbol %= d_symbols_per_bit;
                }
        }
    else
        {
            if (d_trk_parameters.track_pilot)
                {
                    d_P_data_accu = d_Prompt_Data[0];
                }
            else
                {
                    d_P_data_accu = *d_Prompt;
                }
        }

    if (d_trk_parameters.track_pilot)
        {
            // If tracking pilot, disable Costas loop
            d_cloop = false;
        }
    else
        {
            d_cloop = true;
        }
}


void dll_pll_veml_tracking_fpga::log_data()
{
    if (d_dump)
        {
            // Dump results to file
            float prompt_I;
            float prompt_Q;
            float tmp_VE;
            float tmp_E;
            float tmp_P;
            float tmp_L;
            float tmp_VL;
            float tmp_float;
            double tmp_double;
            uint64_t tmp_long_int;
            if (d_trk_parameters.track_pilot)
                {
                    prompt_I = d_Prompt_Data.data()->real();
                    prompt_Q = d_Prompt_Data.data()->imag();
                }
            else
                {
                    prompt_I = d_Prompt->real();
                    prompt_Q = d_Prompt->imag();
                }
            if (d_veml)
                {
                    tmp_VE = std::abs<float>(d_VE_accu);
                    tmp_VL = std::abs<float>(d_VL_accu);
                }
            else
                {
                    tmp_VE = 0.0;
                    tmp_VL = 0.0;
                }
            tmp_E = std::abs<float>(d_E_accu);
            tmp_P = std::abs<float>(d_P_accu);
            tmp_L = std::abs<float>(d_L_accu);

            try
                {
                    // Dump correlators output
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_VE), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_E), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_P), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_L), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_VL), sizeof(float));
                    // PROMPT I and Q (to analyze navigation symbols)
                    d_dump_file.write(reinterpret_cast<char *>(&prompt_I), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char *>(&prompt_Q), sizeof(float));
                    // PRN start sample stamp
                    tmp_long_int = d_sample_counter_next;
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_long_int), sizeof(uint64_t));
                    // accumulated carrier phase
                    tmp_float = static_cast<float>(d_acc_carrier_phase_rad);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // carrier and code frequency
                    tmp_float = static_cast<float>(d_carrier_doppler_hz);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // carrier phase rate [Hz/s]
                    tmp_float = static_cast<float>(d_carrier_phase_rate_step_rad * d_trk_parameters.fs_in * d_trk_parameters.fs_in / TWO_PI);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    tmp_float = static_cast<float>(d_code_freq_chips);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // code phase rate [chips/s^2]
                    tmp_float = static_cast<float>(d_code_phase_rate_step_chips * d_trk_parameters.fs_in * d_trk_parameters.fs_in);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // PLL commands
                    tmp_float = static_cast<float>(d_carr_phase_error_hz);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    tmp_float = static_cast<float>(d_carr_error_filt_hz);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // DLL commands
                    tmp_float = static_cast<float>(d_code_error_chips);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    tmp_float = static_cast<float>(d_code_error_filt_chips);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // CN0 and carrier lock test
                    tmp_float = static_cast<float>(d_CN0_SNV_dB_Hz);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    tmp_float = static_cast<float>(d_carrier_lock_test);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    // AUX vars (for debug purposes)
                    tmp_float = static_cast<float>(d_rem_code_phase_samples);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_float), sizeof(float));
                    tmp_double = static_cast<double>(d_sample_counter_next);
                    d_dump_file.write(reinterpret_cast<char *>(&tmp_double), sizeof(double));
                    // PRN
                    uint32_t prn_ = d_acquisition_gnss_synchro->PRN;
                    d_dump_file.write(reinterpret_cast<char *>(&prn_), sizeof(uint32_t));
                }
            catch (const std::ofstream::failure &e)
                {
                    LOG(WARNING) << "Exception writing trk dump file " << e.what();
                }
        }
}


int32_t dll_pll_veml_tracking_fpga::save_matfile() const
{
    // READ DUMP FILE
    std::ifstream::pos_type size;
    const int32_t number_of_double_vars = 1;
    const int32_t number_of_float_vars = 19;
    const int32_t epoch_size_bytes = sizeof(uint64_t) + sizeof(double) * number_of_double_vars +
                                     sizeof(float) * number_of_float_vars + sizeof(uint32_t);
    std::ifstream dump_file;
    std::string dump_filename_ = d_dump_filename;
    // add channel number to the filename
    dump_filename_.append(std::to_string(d_channel));
    // add extension
    dump_filename_.append(".dat");
    std::cout << "Generating .mat file for " << dump_filename_ << '\n';
    dump_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    try
        {
            dump_file.open(dump_filename_.c_str(), std::ios::binary | std::ios::ate);
        }
    catch (const std::ifstream::failure &e)
        {
            std::cerr << "Problem opening dump file:" << e.what() << '\n';
            return 1;
        }
    // count number of epochs and rewind
    int64_t num_epoch = 0;
    if (dump_file.is_open())
        {
            size = dump_file.tellg();
            num_epoch = static_cast<int64_t>(size) / static_cast<int64_t>(epoch_size_bytes);
            dump_file.seekg(0, std::ios::beg);
        }
    else
        {
            return 1;
        }
    auto abs_VE = std::vector<float>(num_epoch);
    auto abs_E = std::vector<float>(num_epoch);
    auto abs_P = std::vector<float>(num_epoch);
    auto abs_L = std::vector<float>(num_epoch);
    auto abs_VL = std::vector<float>(num_epoch);
    auto Prompt_I = std::vector<float>(num_epoch);
    auto Prompt_Q = std::vector<float>(num_epoch);
    auto PRN_start_sample_count = std::vector<uint64_t>(num_epoch);
    auto acc_carrier_phase_rad = std::vector<float>(num_epoch);
    auto carrier_doppler_hz = std::vector<float>(num_epoch);
    auto carrier_doppler_rate_hz = std::vector<float>(num_epoch);
    auto code_freq_chips = std::vector<float>(num_epoch);
    auto code_freq_rate_chips = std::vector<float>(num_epoch);
    auto carr_error_hz = std::vector<float>(num_epoch);
    auto carr_error_filt_hz = std::vector<float>(num_epoch);
    auto code_error_chips = std::vector<float>(num_epoch);
    auto code_error_filt_chips = std::vector<float>(num_epoch);
    auto CN0_SNV_dB_Hz = std::vector<float>(num_epoch);
    auto carrier_lock_test = std::vector<float>(num_epoch);
    auto aux1 = std::vector<float>(num_epoch);
    auto aux2 = std::vector<double>(num_epoch);
    auto PRN = std::vector<uint32_t>(num_epoch);
    try
        {
            if (dump_file.is_open())
                {
                    for (int64_t i = 0; i < num_epoch; i++)
                        {
                            dump_file.read(reinterpret_cast<char *>(&abs_VE[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&abs_E[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&abs_P[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&abs_L[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&abs_VL[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&Prompt_I[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&Prompt_Q[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&PRN_start_sample_count[i]), sizeof(uint64_t));
                            dump_file.read(reinterpret_cast<char *>(&acc_carrier_phase_rad[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&carrier_doppler_hz[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&carrier_doppler_rate_hz[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&code_freq_chips[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&code_freq_rate_chips[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&carr_error_hz[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&carr_error_filt_hz[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&code_error_chips[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&code_error_filt_chips[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&CN0_SNV_dB_Hz[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&carrier_lock_test[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&aux1[i]), sizeof(float));
                            dump_file.read(reinterpret_cast<char *>(&aux2[i]), sizeof(double));
                            dump_file.read(reinterpret_cast<char *>(&PRN[i]), sizeof(uint32_t));
                        }
                }
            dump_file.close();
        }
    catch (const std::ifstream::failure &e)
        {
            std::cerr << "Problem reading dump file:" << e.what() << '\n';
            return 1;
        }

    // WRITE MAT FILE
    mat_t *matfp;
    matvar_t *matvar;
    std::string filename = std::move(dump_filename_);
    filename.erase(filename.length() - 4, 4);
    filename.append(".mat");
    matfp = Mat_CreateVer(filename.c_str(), nullptr, MAT_FT_MAT73);
    if (reinterpret_cast<int64_t *>(matfp) != nullptr)
        {
            std::array<size_t, 2> dims{1, static_cast<size_t>(num_epoch)};
            matvar = Mat_VarCreate("abs_VE", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), abs_VE.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("abs_E", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), abs_E.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("abs_P", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), abs_P.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("abs_L", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), abs_L.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("abs_VL", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), abs_VL.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("Prompt_I", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), Prompt_I.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("Prompt_Q", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), Prompt_Q.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("PRN_start_sample_count", MAT_C_UINT64, MAT_T_UINT64, 2, dims.data(), PRN_start_sample_count.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("acc_carrier_phase_rad", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), acc_carrier_phase_rad.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("carrier_doppler_hz", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), carrier_doppler_hz.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("carrier_doppler_rate_hz", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), carrier_doppler_rate_hz.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("code_freq_chips", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), code_freq_chips.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("code_freq_rate_chips", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), code_freq_rate_chips.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("carr_error_hz", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), carr_error_hz.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("carr_error_filt_hz", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), carr_error_filt_hz.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("code_error_chips", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), code_error_chips.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("code_error_filt_chips", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), code_error_filt_chips.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("CN0_SNV_dB_Hz", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), CN0_SNV_dB_Hz.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("carrier_lock_test", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), carrier_lock_test.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("aux1", MAT_C_SINGLE, MAT_T_SINGLE, 2, dims.data(), aux1.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("aux2", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims.data(), aux2.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("PRN", MAT_C_UINT32, MAT_T_UINT32, 2, dims.data(), PRN.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);
        }
    Mat_Close(matfp);
    return 0;
}


void dll_pll_veml_tracking_fpga::set_channel(uint32_t channel, const std::string &device_io_name)
{
    gr::thread::scoped_lock l(d_setlock);

    d_channel = channel;
    d_multicorrelator_fpga->open_channel(device_io_name, channel);
    LOG(INFO) << "Tracking Channel set to " << d_channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump)
        {
            std::string dump_filename_ = d_dump_filename;
            // add channel number to the filename
            dump_filename_.append(std::to_string(d_channel));
            // add extension
            dump_filename_.append(".dat");

            if (!d_dump_file.is_open())
                {
                    try
                        {
                            d_dump_file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
                            d_dump_file.open(dump_filename_.c_str(), std::ios::out | std::ios::binary);
                            LOG(INFO) << "Tracking dump enabled on channel " << d_channel << " Log file: " << dump_filename_.c_str();
                        }
                    catch (const std::ofstream::failure &e)
                        {
                            LOG(WARNING) << "channel " << d_channel << " Exception opening trk dump file " << e.what();
                        }
                }
        }
    if (d_enable_extended_integration == true)
        {
            if (d_extended_correlation_in_fpga == true)
                {
                    // Now we can write the secondary codes that do not depend on the PRN number
                    if (d_trk_parameters.system == 'G')
                        {
                            if (d_signal_type == "L5")
                                {
                                    if (d_trk_parameters.track_pilot)
                                        {
                                            d_multicorrelator_fpga->set_secondary_code_lengths(d_secondary_code_length, d_data_secondary_code_length);
                                            d_multicorrelator_fpga->initialize_secondary_code(0, &d_secondary_code_string);
                                            d_multicorrelator_fpga->initialize_secondary_code(1, &d_data_secondary_code_string);
                                        }
                                    else
                                        {
                                            d_multicorrelator_fpga->set_secondary_code_lengths(d_secondary_code_length, 0);
                                            d_multicorrelator_fpga->initialize_secondary_code(0, &d_secondary_code_string);
                                        }
                                }
                        }
                    else if (d_trk_parameters.system == 'E')
                        {
                            if (d_signal_type == "5X")
                                {
                                    // coherent integration in the FPGA is only enabled when tracking the pilot.
                                    if (d_trk_parameters.track_pilot)
                                        {
                                            d_multicorrelator_fpga->set_secondary_code_lengths(d_secondary_code_length, d_data_secondary_code_length);
                                            d_multicorrelator_fpga->initialize_secondary_code(1, &d_data_secondary_code_string);
                                        }
                                }
                        }
                }
        }
}


void dll_pll_veml_tracking_fpga::set_gnss_synchro(Gnss_Synchro *p_gnss_synchro)
{
    d_acquisition_gnss_synchro = p_gnss_synchro;
    if (p_gnss_synchro->PRN > 0)
        {
            gr::thread::scoped_lock lock(d_setlock);
            // A set_gnss_synchro command with a valid PRN is received when the system is going to run acquisition with that PRN.
            // We can use this command to pre-initialize tracking parameters and variables before the actual acquisition process takes place.
            // In this way we minimize the latency between acquisition and tracking once the acquisition has been made.
            d_sample_counter = 0;
            d_sample_counter_next = 0;

            d_carrier_phase_rate_step_rad = 0.0;

            d_code_ph_history.clear();
            d_carr_ph_history.clear();

            if ((d_systemName == "GPS" and d_signal_type == "L5") || (d_systemName == "Galileo" and d_signal_type == "1B"))
                {
                    if (d_trk_parameters.track_pilot)
                        {
                            d_Prompt_Data[0] = gr_complex(0.0, 0.0);
                        }
                }
            else if (d_systemName == "Galileo" and d_signal_type == "5X")
                {
                    if (d_trk_parameters.track_pilot)
                        {
                            d_secondary_code_string = GALILEO_E5A_Q_SECONDARY_CODE[d_acquisition_gnss_synchro->PRN - 1];

                            d_Prompt_Data[0] = gr_complex(0.0, 0.0);

                            if (d_enable_extended_integration == true)
                                {
                                    if (d_extended_correlation_in_fpga == true)
                                        {
                                            d_multicorrelator_fpga->initialize_secondary_code(0, &d_secondary_code_string);
                                        }
                                }
                        }
                }

            std::fill_n(d_correlator_outs.begin(), d_n_correlator_taps, gr_complex(0.0, 0.0));

            d_carrier_lock_fail_counter = 0;
            d_code_lock_fail_counter = 0;
            d_rem_code_phase_samples = 0.0;
            d_rem_carr_phase_rad = 0.0;
            d_rem_code_phase_chips = 0.0;
            d_acc_carrier_phase_rad = 0.0;
            d_cn0_estimation_counter = 0;
            d_carrier_lock_test = 1.0;
            d_CN0_SNV_dB_Hz = 0.0;

            d_code_phase_rate_step_chips = 0.0;

            if (d_veml)
                {
                    d_local_code_shift_chips[0] = -d_trk_parameters.very_early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                    d_local_code_shift_chips[1] = -d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                    d_local_code_shift_chips[3] = d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                    d_local_code_shift_chips[4] = d_trk_parameters.very_early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                }
            else
                {
                    d_local_code_shift_chips[0] = -d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                    d_local_code_shift_chips[2] = d_trk_parameters.early_late_space_chips * static_cast<float>(d_code_samples_per_chip);
                }

            d_current_correlation_time_s = d_code_period;

            // DLL/PLL filter initialization
            d_carrier_loop_filter.set_params(d_trk_parameters.fll_bw_hz, d_trk_parameters.pll_bw_hz, d_trk_parameters.pll_filter_order);
            d_code_loop_filter.set_noise_bandwidth(d_trk_parameters.dll_bw_hz);
            d_code_loop_filter.set_update_interval(static_cast<float>(d_code_period));
            d_code_loop_filter.initialize();  // initialize the code filter

            d_multicorrelator_fpga->set_local_code_and_taps(d_local_code_shift_chips.data(), d_prompt_data_shift, d_acquisition_gnss_synchro->PRN);

            d_pull_in_transitory = true;

            d_cloop = true;

            d_Prompt_circular_buffer.clear();

            d_T_chip_seconds = 1.0 / d_code_freq_chips;
            d_T_prn_seconds = d_T_chip_seconds * static_cast<double>(d_code_length_chips);

            // re-establish nominal integration length (not extended integration by default)
            d_current_integration_length_samples = static_cast<int32_t>(d_trk_parameters.vector_length);
            d_next_integration_length_samples = d_current_integration_length_samples;

            d_multicorrelator_fpga->disable_secondary_codes();  // make sure the processing of the secondary codes is disabled by default

            d_current_fpga_integration_period = 1;
            d_current_extended_correlation_in_fpga = false;

            d_cn0_smoother.reset();
            d_carrier_lock_test_smoother.reset();
        }
}


void dll_pll_veml_tracking_fpga::stop_tracking()
{
    // interrupt the tracking loops
    d_stop_tracking = true;
    // let the samples pass through
    d_multicorrelator_fpga->unlock_channel();
}


void dll_pll_veml_tracking_fpga::reset()
{
    gr::thread::scoped_lock l(d_setlock);
    d_multicorrelator_fpga->unlock_channel();
}


int dll_pll_veml_tracking_fpga::general_work(int noutput_items __attribute__((unused)),
    gr_vector_int &ninput_items __attribute__((unused)),
    gr_vector_const_void_star &input_items __attribute__((unused)),
    gr_vector_void_star &output_items)
{
    gr::thread::scoped_lock l(d_setlock);
    auto **out = reinterpret_cast<Gnss_Synchro **>(&output_items[0]);
    Gnss_Synchro current_synchro_data = Gnss_Synchro();
    current_synchro_data.Flag_valid_symbol_output = false;
    bool loss_of_lock = false;

    while ((!current_synchro_data.Flag_valid_symbol_output) && (!d_stop_tracking))
        {
            d_current_integration_length_samples = d_next_integration_length_samples;

            if (d_pull_in_transitory == true)
                {
                    if (d_sample_counter > 0)  // do not execute this condition until the sample counter has ben read for the first time after start_tracking
                        {
                            if (d_trk_parameters.pull_in_time_s < (d_sample_counter - d_acq_sample_stamp) / static_cast<int>(d_trk_parameters.fs_in))
                                {
                                    d_pull_in_transitory = false;
                                    d_carrier_lock_fail_counter = 0;
                                    d_code_lock_fail_counter = 0;
                                }
                        }
                }
            switch (d_state)
                {
                case 1:  // Pull-in
                    {
                        boost::mutex::scoped_lock lock(d_mutex);
                        d_worker_is_done = false;
                        l.unlock();
                        while (!d_worker_is_done)
                            {
                                d_m_condition.wait(lock);
                            }
                        l.lock();
                        // Signal alignment (skip samples until the incoming signal is aligned with local replica)
                        int64_t acq_trk_diff_samples;
                        double acq_trk_diff_seconds;
                        double delta_trk_to_acq_prn_start_samples;
                        uint64_t absolute_samples_offset;

                        d_multicorrelator_fpga->lock_channel();
                        const uint64_t counter_value = d_multicorrelator_fpga->read_sample_counter();
                        if (counter_value > (d_acq_sample_stamp + d_acq_code_phase_samples))
                            {
                                // Signal alignment (skip samples until the incoming signal is aligned with local replica)
                                acq_trk_diff_samples = static_cast<int64_t>(counter_value) - static_cast<int64_t>(d_acq_sample_stamp);
                                acq_trk_diff_seconds = static_cast<double>(acq_trk_diff_samples) / d_trk_parameters.fs_in;
                                delta_trk_to_acq_prn_start_samples = static_cast<double>(acq_trk_diff_samples) - d_acq_code_phase_samples;

                                const uint32_t num_frames = ceil((delta_trk_to_acq_prn_start_samples) / d_current_integration_length_samples);
                                absolute_samples_offset = static_cast<uint64_t>(d_acq_code_phase_samples + d_acq_sample_stamp + num_frames * d_current_integration_length_samples);
                            }
                        else
                            {
                                // test mode
                                acq_trk_diff_samples = -static_cast<int64_t>(counter_value) + static_cast<int64_t>(d_acq_sample_stamp);
                                acq_trk_diff_seconds = static_cast<double>(acq_trk_diff_samples) / d_trk_parameters.fs_in;
                                delta_trk_to_acq_prn_start_samples = static_cast<double>(acq_trk_diff_samples) + d_acq_code_phase_samples;

                                absolute_samples_offset = static_cast<uint64_t>(delta_trk_to_acq_prn_start_samples);
                            }

                        d_multicorrelator_fpga->set_initial_sample(absolute_samples_offset);
                        d_sample_counter = absolute_samples_offset;
                        d_sample_counter_next = d_sample_counter;

                        // Doppler effect Fd = (C / (C + Vr)) * F
                        const double radial_velocity = (d_signal_carrier_freq + d_acq_carrier_doppler_hz) / d_signal_carrier_freq;
                        // new chip and PRN sequence periods based on acq Doppler
                        d_code_freq_chips = radial_velocity * d_code_chip_rate;
                        d_code_phase_step_chips = d_code_freq_chips / d_trk_parameters.fs_in;

                        d_acq_code_phase_samples = absolute_samples_offset;

                        const int32_t samples_offset = round(d_acq_code_phase_samples);
                        d_acc_carrier_phase_rad -= d_carrier_phase_step_rad * static_cast<double>(samples_offset);

                        d_state = 2;

                        LOG(INFO) << "Number of samples between Acquisition and Tracking = " << acq_trk_diff_samples << " ( " << acq_trk_diff_seconds << " s)";
                        DLOG(INFO) << "PULL-IN Doppler [Hz] = " << d_carrier_doppler_hz
                                   << ". PULL-IN Code Phase [samples] = " << d_acq_code_phase_samples;

                        // DEBUG OUTPUT
                        std::cout << "Tracking of " << d_systemName << " " << d_signal_pretty_name << " signal started on channel " << d_channel << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                        DLOG(INFO) << "Starting tracking of satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << " on channel " << d_channel;

                        // DLOG(INFO) << "Number of samples between Acquisition and Tracking = " << acq_trk_diff_samples << " ( " << acq_trk_diff_seconds << " s)";
                        // std::cout << "Number of samples between Acquisition and Tracking = " << acq_trk_diff_samples << " ( " << acq_trk_diff_seconds << " s)\n";
                        // DLOG(INFO) << "PULL-IN Doppler [Hz] = " << d_carrier_doppler_hz
                        //            << ". PULL-IN Code Phase [samples] = " << d_acq_code_phase_samples;

                        break;
                    }
                case 2:  // Wide tracking and symbol synchronization
                    {
                        d_sample_counter = d_sample_counter_next;
                        d_sample_counter_next = d_sample_counter + static_cast<uint64_t>(d_current_integration_length_samples);

                        do_correlation_step();

                        // Save single correlation step variables
                        if (d_veml)
                            {
                                d_VE_accu = *d_Very_Early;
                                d_VL_accu = *d_Very_Late;
                            }
                        d_E_accu = *d_Early;
                        d_P_accu = *d_Prompt;
                        d_L_accu = *d_Late;
                        d_trk_parameters.spc = d_trk_parameters.early_late_space_chips;
                        // if (std::string(d_trk_parameters.signal) == "E1")
                        //    {
                        //        d_trk_parameters.slope = -CalculateSlopeAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc);
                        //        d_trk_parameters.y_intercept = GetYInterceptAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc);
                        //    }

                        // fail-safe: check if the secondary code or bit synchronization has not succeeded in a limited time period
                        if (d_trk_parameters.bit_synchronization_time_limit_s < (d_sample_counter - d_acq_sample_stamp) / static_cast<int>(d_trk_parameters.fs_in))
                            {
                                d_carrier_lock_fail_counter = 300000;  // force loss-of-lock condition
                                LOG(INFO) << d_systemName << " " << d_signal_pretty_name << " tracking synchronization time limit reached in channel " << d_channel
                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                            }

                        // Check lock status
                        if (!cn0_and_tracking_lock_status(d_code_period))
                            {
                                clear_tracking_vars();
                                d_state = 1;                                         // loss-of-lock detected
                                loss_of_lock = true;                                 // Set the flag so that the negative indication can be generated
                                current_synchro_data = *d_acquisition_gnss_synchro;  // Fill in the Gnss_Synchro object with basic info
                            }
                        else
                            {
                                bool next_state = false;

                                // Perform DLL/PLL tracking loop computations. Costas Loop enabled
                                run_dll_pll();

                                update_tracking_vars();

                                // enable write dump file this cycle (valid DLL/PLL cycle)
                                log_data();

                                if (!d_pull_in_transitory)
                                    {
                                        if (d_secondary)
                                            {
                                                // ####### SECONDARY CODE LOCK #####
                                                d_Prompt_circular_buffer.push_back(*d_Prompt);

                                                if (d_Prompt_circular_buffer.size() == d_secondary_code_length)
                                                    {
                                                        next_state = acquire_secondary();

                                                        if (next_state)
                                                            {
                                                                LOG(INFO) << d_systemName << " " << d_signal_pretty_name << " secondary code locked in channel " << d_channel
                                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                                                                std::cout << d_systemName << " " << d_signal_pretty_name << " secondary code locked in channel " << d_channel
                                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                                                            }
                                                    }
                                            }
                                        else if (d_symbols_per_bit > 1)  // Signal does not have secondary code. Search a bit transition by sign change
                                            {
                                                // ******* preamble correlation ********
                                                d_Prompt_circular_buffer.push_back(*d_Prompt);
                                                if (d_Prompt_circular_buffer.size() == d_secondary_code_length)
                                                    {
                                                        next_state = acquire_secondary();
                                                        if (next_state)
                                                            {
                                                                LOG(INFO) << d_systemName << " " << d_signal_pretty_name << " tracking bit synchronization locked in channel " << d_channel
                                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN);
                                                                std::cout << d_systemName << " " << d_signal_pretty_name << " tracking bit synchronization locked in channel " << d_channel
                                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                                                            }
                                                    }
                                            }
                                        else
                                            {
                                                next_state = true;
                                            }
                                    }
                                else
                                    {
                                        next_state = false;  // keep in state 2 during pull-in transitory
                                    }

                                if (next_state)
                                    {  // reset extended correlator
                                        d_VE_accu = gr_complex(0.0, 0.0);
                                        d_E_accu = gr_complex(0.0, 0.0);
                                        d_P_accu = gr_complex(0.0, 0.0);
                                        d_P_data_accu = gr_complex(0.0, 0.0);
                                        d_L_accu = gr_complex(0.0, 0.0);
                                        d_VL_accu = gr_complex(0.0, 0.0);
                                        d_Prompt_circular_buffer.clear();
                                        d_current_symbol = 0;
                                        d_current_data_symbol = 0;

                                        if (d_enable_extended_integration)
                                            {
                                                // update integration time
                                                d_extend_correlation_symbols_count = 0;
                                                d_current_correlation_time_s = static_cast<float>(d_trk_parameters.extend_correlation_symbols) * static_cast<float>(d_code_period);

                                                if (d_extended_correlation_in_fpga)
                                                    {
                                                        d_current_fpga_integration_period = d_fpga_integration_period;
                                                        d_current_extended_correlation_in_fpga = true;

                                                        if (d_sc_demodulate_enabled)
                                                            {
                                                                d_multicorrelator_fpga->enable_secondary_codes();
                                                            }

                                                        if (d_extend_fpga_integration_periods > 1)
                                                            {
                                                                // correction on already computed parameters
                                                                d_K_blk_samples = d_T_prn_samples * (d_fpga_integration_period) + d_rem_code_phase_samples_prev;
                                                                d_next_integration_length_samples = static_cast<int32_t>(std::floor(d_K_blk_samples));
                                                                d_state = 5;
                                                            }
                                                        else
                                                            {
                                                                // correction on already computed parameters
                                                                d_K_blk_samples = d_T_prn_samples * d_trk_parameters.extend_correlation_symbols + d_rem_code_phase_samples_prev;
                                                                d_next_integration_length_samples = static_cast<int32_t>(std::floor(d_K_blk_samples));
                                                                d_state = 6;
                                                            }
                                                    }
                                                else
                                                    {
                                                        d_state = 3;  // next state is the extended correlator integrator
                                                    }

                                                LOG(INFO) << "Enabled " << d_trk_parameters.extend_correlation_symbols * static_cast<int32_t>(d_code_period * 1000.0) << " ms extended correlator in channel "
                                                          << d_channel
                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN);
                                                std::cout << "Enabled " << d_trk_parameters.extend_correlation_symbols * static_cast<int32_t>(d_code_period * 1000.0) << " ms extended correlator in channel "
                                                          << d_channel
                                                          << " for satellite " << Gnss_Satellite(d_systemName, d_acquisition_gnss_synchro->PRN) << '\n';
                                                // Set narrow taps delay values [chips]
                                                d_code_loop_filter.set_update_interval(static_cast<float>(d_current_correlation_time_s));
                                                d_code_loop_filter.set_noise_bandwidth(d_trk_parameters.dll_bw_narrow_hz);
                                                d_carrier_loop_filter.set_params(d_trk_parameters.fll_bw_hz, d_trk_parameters.pll_bw_narrow_hz, d_trk_parameters.pll_filter_order);
                                                if (d_veml)
                                                    {
                                                        d_local_code_shift_chips[0] = -d_trk_parameters.very_early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_local_code_shift_chips[1] = -d_trk_parameters.early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_local_code_shift_chips[3] = d_trk_parameters.early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_local_code_shift_chips[4] = d_trk_parameters.very_early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_trk_parameters.spc = d_trk_parameters.early_late_space_narrow_chips;
                                                        // d_trk_parameters.slope = -CalculateSlopeAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc);
                                                        // d_trk_parameters.y_intercept = GetYInterceptAbs(&SinBocCorrelationFunction<1, 1>, d_trk_parameters.spc);
                                                    }
                                                else
                                                    {
                                                        d_local_code_shift_chips[0] = -d_trk_parameters.early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_local_code_shift_chips[2] = d_trk_parameters.early_late_space_narrow_chips * static_cast<float>(d_code_samples_per_chip);
                                                        d_trk_parameters.spc = d_trk_parameters.early_late_space_narrow_chips;
                                                    }
                                            }
                                        else
                                            {
                                                d_state = 4;
                                            }
                                    }
                            }
                        break;
                    }
                case 3:  // coherent integration (correlation time extension)
                    {
                        d_sample_counter = d_sample_counter_next;
                        d_sample_counter_next = d_sample_counter + static_cast<uint64_t>(d_current_integration_length_samples);

                        // perform a correlation step
                        do_correlation_step();
                        save_correlation_results();
                        update_tracking_vars();

                        if (d_current_data_symbol == 0)
                            {
                                log_data();
                                // ########### Output the tracking results to Telemetry block ##########
                                // Fill the acquisition data
                                current_synchro_data = *d_acquisition_gnss_synchro;
                                if (d_interchange_iq)
                                    {
                                        current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.imag());
                                        current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.real());
                                    }
                                else
                                    {
                                        current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.real());
                                        current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.imag());
                                    }
                                current_synchro_data.Code_phase_samples = d_rem_code_phase_samples;
                                current_synchro_data.Carrier_phase_rads = d_acc_carrier_phase_rad;
                                current_synchro_data.Carrier_Doppler_hz = d_carrier_doppler_hz;
                                current_synchro_data.CN0_dB_hz = d_CN0_SNV_dB_Hz;
                                current_synchro_data.correlation_length_ms = d_correlation_length_ms;
                                current_synchro_data.Flag_valid_symbol_output = true;
                                d_P_data_accu = gr_complex(0.0, 0.0);
                            }

                        d_extend_correlation_symbols_count++;
                        if (d_extend_correlation_symbols_count == (d_trk_parameters.extend_correlation_symbols - 1))
                            {
                                d_extend_correlation_symbols_count = 0;
                                d_state = 4;
                            }
                        break;
                    }
                case 4:  // narrow tracking
                    {
                        d_sample_counter = d_sample_counter_next;
                        d_sample_counter_next = d_sample_counter + static_cast<uint64_t>(d_current_integration_length_samples);

                        // perform a correlation step
                        do_correlation_step();
                        save_correlation_results();

                        // check lock status
                        if (!cn0_and_tracking_lock_status(d_code_period * static_cast<double>(d_trk_parameters.extend_correlation_symbols)))
                            {
                                clear_tracking_vars();
                                d_state = 1;                                         // loss-of-lock detected
                                loss_of_lock = true;                                 // Set the flag so that the negative indication can be generated
                                current_synchro_data = *d_acquisition_gnss_synchro;  // Fill in the Gnss_Synchro object with basic info
                            }
                        else
                            {
                                run_dll_pll();
                                update_tracking_vars();
                                check_carrier_phase_coherent_initialization();
                                if (d_current_data_symbol == 0)
                                    {
                                        // enable write dump file this cycle (valid DLL/PLL cycle)
                                        log_data();
                                        // ########### Output the tracking results to Telemetry block ##########
                                        // Fill the acquisition data
                                        current_synchro_data = *d_acquisition_gnss_synchro;
                                        if (d_interchange_iq)
                                            {
                                                current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.imag());
                                                current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.real());
                                            }
                                        else
                                            {
                                                current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.real());
                                                current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.imag());
                                            }
                                        current_synchro_data.Code_phase_samples = d_rem_code_phase_samples;
                                        current_synchro_data.Carrier_phase_rads = d_acc_carrier_phase_rad;
                                        current_synchro_data.Carrier_Doppler_hz = d_carrier_doppler_hz;
                                        current_synchro_data.CN0_dB_hz = d_CN0_SNV_dB_Hz;
                                        current_synchro_data.correlation_length_ms = d_correlation_length_ms;
                                        current_synchro_data.Flag_valid_symbol_output = true;
                                        d_P_data_accu = gr_complex(0.0, 0.0);
                                    }

                                // reset extended correlator
                                d_VE_accu = gr_complex(0.0, 0.0);
                                d_E_accu = gr_complex(0.0, 0.0);
                                d_P_accu = gr_complex(0.0, 0.0);
                                d_L_accu = gr_complex(0.0, 0.0);
                                d_VL_accu = gr_complex(0.0, 0.0);
                                if (d_enable_extended_integration)
                                    {
                                        d_state = 3;  // new coherent integration (correlation time extension) cycle
                                    }
                            }
                        break;
                    }
                case 5:  // coherent integration (correlation time extension)
                    {
                        d_sample_counter = d_sample_counter_next;
                        d_sample_counter_next = d_sample_counter + static_cast<uint64_t>(d_current_integration_length_samples);

                        // this must be computed for the secondary prn code
                        if (d_secondary)
                            {
                                const uint32_t next_prn_length = d_current_integration_length_samples / d_fpga_integration_period;
                                const uint32_t first_prn_length = d_current_integration_length_samples - next_prn_length * (d_fpga_integration_period - 1);

                                d_multicorrelator_fpga->update_prn_code_length(first_prn_length, next_prn_length);
                            }

                        // perform a correlation step
                        do_correlation_step();
                        save_correlation_results();
                        update_tracking_vars();

                        if (d_current_data_symbol == 0)
                            {
                                log_data();
                                // ########### Output the tracking results to Telemetry block ##########
                                // Fill the acquisition data
                                current_synchro_data = *d_acquisition_gnss_synchro;
                                if (d_interchange_iq)
                                    {
                                        current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.imag());
                                        current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.real());
                                    }
                                else
                                    {
                                        current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.real());
                                        current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.imag());
                                    }
                                current_synchro_data.Code_phase_samples = d_rem_code_phase_samples;
                                current_synchro_data.Carrier_phase_rads = d_acc_carrier_phase_rad;
                                current_synchro_data.Carrier_Doppler_hz = d_carrier_doppler_hz;
                                current_synchro_data.CN0_dB_hz = d_CN0_SNV_dB_Hz;
                                current_synchro_data.correlation_length_ms = d_correlation_length_ms;
                                current_synchro_data.Flag_valid_symbol_output = true;
                                d_P_data_accu = gr_complex(0.0, 0.0);
                            }

                        d_extend_correlation_symbols_count++;
                        if (d_extend_correlation_symbols_count == (d_extend_fpga_integration_periods - 1))
                            {
                                d_extend_correlation_symbols_count = 0;
                                d_state = 6;
                            }

                        break;
                    }
                case 6:  // narrow tracking IN THE FPGA
                    {
                        d_sample_counter = d_sample_counter_next;
                        d_sample_counter_next = d_sample_counter + static_cast<uint64_t>(d_current_integration_length_samples);

                        // this must be computed for the secondary prn code
                        if (d_secondary)
                            {
                                const uint32_t next_prn_length = d_current_integration_length_samples / d_fpga_integration_period;
                                const uint32_t first_prn_length = d_current_integration_length_samples - next_prn_length * (d_fpga_integration_period - 1);

                                d_multicorrelator_fpga->update_prn_code_length(first_prn_length, next_prn_length);
                            }

                        // perform a correlation step
                        do_correlation_step();
                        save_correlation_results();
                        // check lock status
                        if (!cn0_and_tracking_lock_status(d_code_period * static_cast<double>(d_trk_parameters.extend_correlation_symbols)))
                            {
                                clear_tracking_vars();
                                d_state = 1;                                         // loss-of-lock detected
                                loss_of_lock = true;                                 // Set the flag so that the negative indication can be generated
                                current_synchro_data = *d_acquisition_gnss_synchro;  // Fill in the Gnss_Synchro object with basic info
                            }
                        else
                            {
                                run_dll_pll();
                                update_tracking_vars();
                                check_carrier_phase_coherent_initialization();

                                if (d_current_data_symbol == 0)
                                    {
                                        // enable write dump file this cycle (valid DLL/PLL cycle)
                                        log_data();
                                        // ########### Output the tracking results to Telemetry block ##########
                                        // Fill the acquisition data
                                        current_synchro_data = *d_acquisition_gnss_synchro;
                                        if (d_interchange_iq)
                                            {
                                                current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.imag());
                                                current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.real());
                                            }
                                        else
                                            {
                                                current_synchro_data.Prompt_I = static_cast<double>(d_P_data_accu.real());
                                                current_synchro_data.Prompt_Q = static_cast<double>(d_P_data_accu.imag());
                                            }
                                        current_synchro_data.Code_phase_samples = d_rem_code_phase_samples;
                                        current_synchro_data.Carrier_phase_rads = d_acc_carrier_phase_rad;
                                        current_synchro_data.Carrier_Doppler_hz = d_carrier_doppler_hz;
                                        current_synchro_data.CN0_dB_hz = d_CN0_SNV_dB_Hz;
                                        current_synchro_data.correlation_length_ms = d_correlation_length_ms;
                                        current_synchro_data.Flag_valid_symbol_output = true;
                                        d_P_data_accu = gr_complex(0.0, 0.0);
                                    }

                                d_extend_correlation_symbols_count = 0;

                                // reset extended correlator
                                d_VE_accu = gr_complex(0.0, 0.0);
                                d_E_accu = gr_complex(0.0, 0.0);
                                d_P_accu = gr_complex(0.0, 0.0);
                                d_L_accu = gr_complex(0.0, 0.0);
                                d_VL_accu = gr_complex(0.0, 0.0);

                                if (d_extend_fpga_integration_periods > 1)
                                    {
                                        d_state = 5;
                                    }
                            }
                        break;
                    }
                default:
                    break;
                }
        }

    if (current_synchro_data.Flag_valid_symbol_output || loss_of_lock)
        {
            current_synchro_data.fs = static_cast<int64_t>(d_trk_parameters.fs_in);
            current_synchro_data.Tracking_sample_counter = d_sample_counter_next;  // d_sample_counter;
            current_synchro_data.Flag_valid_symbol_output = !loss_of_lock;
            current_synchro_data.Flag_PLL_180_deg_phase_locked = d_Flag_PLL_180_deg_phase_locked;
            *out[0] = std::move(current_synchro_data);
            return 1;
        }
    return 0;
}
