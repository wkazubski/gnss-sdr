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
#include "signal_source_base.h"
#include <gnuradio/blocks/file_sink.h>
#include <pmt/pmt.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

/** \addtogroup Signal_Source
 * \{ */
/** \addtogroup Signal_Source_adapters
 * \{ */


class ConfigurationInterface;

/*!
 * \brief This class reads samples from a gnMAX2769 USB dongle, a RF front-end signal sampler
 */
class GnMaxSignalSource : public SignalSourceBase
{
public:
    GnMaxSignalSource(const ConfigurationInterface* configuration,
        const std::string& role, unsigned int in_stream,
        unsigned int out_stream, Concurrent_Queue<pmt::pmt_t>* queue);

    virtual ~GnMaxSignalSource();

    inline size_t item_size() override
    {
        return item_size_;
    }

    void connect(gr::top_block_sptr top_block) override;
    void disconnect(gr::top_block_sptr top_block) override;
    gr::basic_block_sptr get_left_block() override;
    gr::basic_block_sptr get_right_block() override;

private:

    gr::block_sptr gnmax_source_;
    gnss_shared_ptr<gr::block> valve_;
    gr::blocks::file_sink::sptr file_sink_;

    std::string item_type_;
    std::string dump_filename_;

    // Front-end settings
    bool bias_;
    int bias__;
    int ant_;
    unsigned long freq_;  // frequency of local oscilator
    float freq__;
    unsigned long bw_;
    int bw__;
    bool zeroif_;
    int zeroif__;

    size_t item_size_;
    int64_t samples_;

    unsigned int in_stream_;
    unsigned int out_stream_;

    bool dump_;
};


/** \} */
/** \} */
#endif /*GNSS_SDR_GNMAX_SIGNAL_SOURCE_H_*/
