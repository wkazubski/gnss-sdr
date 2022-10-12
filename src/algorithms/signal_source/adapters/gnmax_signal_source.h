/*!
 * \file gnmax_signal_source.h
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


#ifndef GNSS_SDR_GNMAX_SIGNAL_SOURCE_H_
#define GNSS_SDR_GNMAX_SIGNAL_SOURCE_H_

#include "concurrent_queue.h"
#include "gnss_block_interface.h"
#include <gnuradio/blocks/file_sink.h>
#include <gnuradio/hier_block2.h>
#include <pmt/pmt.h>
#include <string>


class ConfigurationInterface;

/*!
 * \brief This class reads samples from a gnMAX2769 USB dongle, a RF front-end signal sampler
 */
class GnMaxSignalSource : public GNSSBlockInterface
{
public:
    GnMaxSignalSource(const ConfigurationInterface* configuration,
        std::string role, unsigned int in_stream,
        unsigned int out_stream, Concurrent_Queue<pmt::pmt_t>* queue);

    virtual ~GnMaxSignalSource();

    inline std::string role() override
    {
        return role_;
    }

    /*!
     * \brief Returns "GNMAX_Signal_Source".
     */
    inline std::string implementation() override
    {
        return "GNMAX_Signal_Source";
    }

    inline size_t item_size() override
    {
        return item_size_;
    }
/*
    inline size_t getRfChannels() const final
    {
        return 0;
    }
*/
    void connect(gr::top_block_sptr top_block) override;
    void disconnect(gr::top_block_sptr top_block) override;
    gr::basic_block_sptr get_left_block() override;
    gr::basic_block_sptr get_right_block() override;

private:
    std::string role_;
    bool bias_;
    int bias__;
    int ant_;
    unsigned long freq_;  // frequency of local oscilator
    float freq__;
    unsigned long bw_;
    int bw__;
    bool zeroif_;
    int zeroif__;
    unsigned int in_stream_;
    unsigned int out_stream_;
    std::string item_type_;
    size_t item_size_;
    long samples_;
    bool dump_;
    std::string dump_filename_;
    gr::block_sptr gnmax_source_;
    gr::blocks::file_sink::sptr file_sink_;
};

#endif /*GNSS_SDR_GNMAX_SIGNAL_SOURCE_H_*/
