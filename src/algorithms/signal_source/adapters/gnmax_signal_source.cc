/*!
 * \file gnmax_signal_source.cc
 * \brief gnMAX2769 USB dongle GPS RF front-end signal sampler driver
 * \author Wojciech Kazubski, wk(at)ire.pw.edu.pl
 * \author Javier Arribas, jarribas(at)cttc.es
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "gnmax_signal_source.h"
#include "GPS_L1_CA.h"
#include "configuration_interface.h"
#include "gnss_sdr_string_literals.h"
#include "gnss_sdr_valve.h"
#include <glog/logging.h>
#include <gnuradio/blocks/file_sink.h>
#include <gnMAX2769/gnmax_source_cc.h>


using namespace std::string_literals;


GnMaxSignalSource::GnMaxSignalSource(const ConfigurationInterface* configuration,
    const std::string& role,
    unsigned int in_stream,
    unsigned int out_stream,
    Concurrent_Queue<pmt::pmt_t>* queue)
    : SignalSourceBase(configuration, role, "GnMax_Signal_Source"s),
      item_type_(configuration->property(role + ".item_type", std::string("gr_complex"))),
      dump_filename_(configuration->property(role + ".dump_filename", std::string("./data/signal_source.dat"))),
      bias_(configuration->property(role + ".antenna_bias", true)),
      ant_(configuration->property(role + ".antenna", 3)),
      freq_(configuration->property(role + ".freq", GPS_L1_FREQ_HZ)),
      bw_(configuration->property(role + ".if_bandwidth", 1)),
      zeroif_(configuration->property(role + ".zero_if", false)),
      in_stream_(in_stream),
      out_stream_(out_stream),
      dump_(configuration->property(role + ".dump", false))
{
    if (bias_)
        bias__ = 1;
    else
        bias__ = 0;

    freq__ = static_cast<float>(freq_);

    if (bw_ <= 2.501E6)
        bw__ = 0;
    else
        {
            if (bw_ <= 4.201E6)
                bw__ = 1;
            else
                {
                    if (bw_ <= 8.001E6)
                        bw__ = 2;
                    else
                        bw__ = 3;
                }
        }

    if (zeroif_)
        zeroif__ = 1;
    else
        zeroif__ = 0;

    if (item_type_.compare("gr_complex") == 0)
        {
            item_size_ = sizeof(gr_complex);
            gnmax_source_ = gr::gnMAX2769::gnmax_source_cc::make(bias__, ant_, freq__, bw__, zeroif__);
            DLOG(INFO) << "Item size " << item_size_;
            DLOG(INFO) << "gnmax_source(" << gnmax_source_->unique_id() << ")";
        }
    //    else if (item_type_.compare("short") == 0)
    //        {
    //            item_size_ = sizeof(short);
    //            resampler_ = direct_resampler_make_conditioner_ss(sample_freq_in_,
    //                    sample_freq_out_);
    //        }
    else
        {
            LOG(WARNING) << item_type_
                         << " unrecognized item type for resampler";
            item_size_ = sizeof(short);
        }

    if (samples_ != 0)
        {
            DLOG(INFO) << "Send STOP signal after " << samples_ << " samples";
            valve_ = gnss_sdr_make_valve(item_size_, samples_, queue);
            DLOG(INFO) << "valve(" << valve_->unique_id() << ")";
        }

    if (dump_)
        {
            DLOG(INFO) << "Dumping output into file " << dump_filename_;
            file_sink_ = gr::blocks::file_sink::make(item_size_, dump_filename_.c_str());
            DLOG(INFO) << "file_sink(" << file_sink_->unique_id() << ")";
        }

    if (in_stream_ > 0)
        {
            LOG(ERROR) << "A signal source does not have an input stream";
        }
    if (out_stream_ > 1)
        {
            LOG(ERROR) << "This implementation only supports one output stream";
        }
}


GnMaxSignalSource::~GnMaxSignalSource()
{
}


void GnMaxSignalSource::connect(gr::top_block_sptr top_block)
{
    if (samples_ != 0)
        {
            top_block->connect(gnmax_source_, 0, valve_, 0);
            DLOG(INFO) << "connected limesdr source to valve";
            if (dump_)
                {
                    top_block->connect(valve_, 0, file_sink_, 0);
                    DLOG(INFO) << "connected valve to file sink";
                }
        }
    else
        {
            if (dump_)
                {
                    top_block->connect(gnmax_source_, 0, file_sink_, 0);
                    DLOG(INFO) << "connected limesdr source to file sink";
                }
        }
}


void GnMaxSignalSource::disconnect(gr::top_block_sptr top_block)
{
    if (samples_ != 0)
        {
            top_block->disconnect(gnmax_source_, 0, valve_, 0);
            if (dump_)
                {
                    top_block->disconnect(valve_, 0, file_sink_, 0);
                }
        }
    else
        {
            if (dump_)
                {
                    top_block->disconnect(gnmax_source_, 0, file_sink_, 0);
                }
        }
}


gr::basic_block_sptr GnMaxSignalSource::get_left_block()
{
    LOG(WARNING) << "Left block of a signal source should not be retrieved";
    return gr::block_sptr();
}


gr::basic_block_sptr GnMaxSignalSource::get_right_block()
{
    if (samples_ != 0)
        {
            return valve_;
        }
    else
        {
            return gnmax_source_;
        }
}
