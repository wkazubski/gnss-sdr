/*!
 * \file gnmax_signal_source.cc
 * \brief gnMAX2769 USB dongle GPS RF front-end signal sampler driver
 * \author Wojciech Kazubski, wk(at)ire.pw.edu.pl
 * \author Javier Arribas, jarribas(at)cttc.es
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2018  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <https://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "gnmax_signal_source.h"
#include "configuration_interface.h"
#include <glog/logging.h>
#include <gnuradio/blocks/file_sink.h>
#include "GPS_L1_CA.h"
#include <gnMAX2769/gnmax_source_cc.h>


GnMaxSignalSource::GnMaxSignalSource(ConfigurationInterface* configuration,
    std::string role, unsigned int in_stream, unsigned int out_stream, std::shared_ptr<Concurrent_Queue<pmt::pmt_t>> queue) : role_(role), in_stream_(in_stream), out_stream_(out_stream), queue_(queue)
{
    std::string default_item_type = "short";
    std::string default_dump_file = "./data/gnmax_source.dat";
    item_type_ = configuration->property(role + ".item_type", default_item_type);
    bias_ = configuration->property(role + ".antenna_bias", true);
    ant_ = configuration->property(role + ".antenna", 3);
    freq_ = configuration->property(role + ".freq", GPS_L1_FREQ_HZ);
    bw_ = configuration->property(role + ".if_bandwidth", 1);
    zeroif_ = configuration->property(role + ".zero_if", false);
    dump_ = configuration->property(role + ".dump", false);
    dump_filename_ = configuration->property(role + ".dump_filename", default_dump_file);

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
    if (dump_)
        {
            DLOG(INFO) << "Dumping output into file " << dump_filename_;
            file_sink_ = gr::blocks::file_sink::make(item_size_, dump_filename_.c_str());
        }
    if (dump_)
        {
            DLOG(INFO) << "file_sink(" << file_sink_->unique_id() << ")";
        }
}


GnMaxSignalSource::~GnMaxSignalSource()
{
}


void GnMaxSignalSource::connect(gr::top_block_sptr top_block)
{
    if (dump_)
        {
            top_block->connect(gnmax_source_, 0, file_sink_, 0);
            DLOG(INFO) << "connected gnmax_source to file sink";
        }
    else
        {
            DLOG(INFO) << "nothing to connect internally";
        }
}


void GnMaxSignalSource::disconnect(gr::top_block_sptr top_block)
{
    if (dump_)
        {
            top_block->disconnect(gnmax_source_, 0, file_sink_, 0);
        }
}


gr::basic_block_sptr GnMaxSignalSource::get_left_block()
{
    LOG(WARNING) << "Left block of a signal source should not be retrieved";
    return gr::block_sptr();
}


gr::basic_block_sptr GnMaxSignalSource::get_right_block()
{
    return gnmax_source_;
}