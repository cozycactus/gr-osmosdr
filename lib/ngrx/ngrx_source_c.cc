/* -*- c++ -*- */
/*
 * Copyright 2012 Dimitri Stolnikov <horiz0n@gmx.net>
 * Copyright 2012 Steve Markgraf <steve@steve-m.de>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ngrx_source_c.h"
#include <gnuradio/io_signature.h>

#include <boost/assign.hpp>
#include <boost/format.hpp>

#include <stdexcept>
#include <iostream>
#include <stdio.h>

#include <libsddc.h>

#include "arg_helpers.h"

using namespace boost::assign;

#define BUF_SIZE  (1024 * 1024)
#define BUF_NUM   15
#define BUF_SKIP  1 // buffers to skip due to garbage

#define BYTES_PER_SAMPLE  8

#define DC_LOOPS 5
#define FW_PATH "/home/vlad/warez/ExtIO_sddc/SDDC_FX3.img"

/*
 * Create a new instance of ngrx_source_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
ngrx_source_c_sptr
make_ngrx_source_c (const std::string &args)
{
  return gnuradio::get_initial_sptr(new ngrx_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;	// mininum number of input streams
static const int MAX_IN = 0;	// maximum number of input streams
static const int MIN_OUT = 1;	// minimum number of output streams
static const int MAX_OUT = 1;	// maximum number of output streams

/*
 * The private constructor
 */
ngrx_source_c::ngrx_source_c (const std::string &args)
  : gr::sync_block ("ngrx_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _running(true),
    _auto_gain(false),
    _skipped(0),
    _freq_corr(0.0),
    _center_freq(100.0e6),
    _dc_offset(0.0),
    _dc_accum(0.0),
    _dc_loops(DC_LOOPS),//disable
    _dc_count(0),
    _dc_size(0)
{
  int dev_index = 0;

  dict_t dict = params_to_dict(args);

  if (dict.count("sddc"))
    dev_index = boost::lexical_cast< unsigned int >( dict["sddc"] );

  _buf_num = _buf_head = _buf_used = _buf_offset = 0;
  _samp_avail = BUF_SIZE / BYTES_PER_SAMPLE;

  if (dict.count("buffers"))
    _buf_num = boost::lexical_cast< unsigned int >( dict["buffers"] );

  if (0 == _buf_num)
    _buf_num = BUF_NUM;

  if ( BUF_NUM != _buf_num ) {
    std::cerr << "Using " << _buf_num << " buffers of size " << BUF_SIZE << "."
              << std::endl;
  }

  if ( dev_index >= sddc_get_device_count() )
    throw std::runtime_error("Wrong sddc device index given.");

  std::cerr << "Using device #" << dev_index << ": "
//            << mirisdr_get_device_name(dev_index)
            << std::endl;

  _dev = NULL;
  _dev = sddc_open( dev_index , FW_PATH);
  if (!_dev)
    throw std::runtime_error("Failed to open sddc device.");
#if 0
  ret = mirisdr_set_sample_rate( _dev, 500000 );
  if (ret < 0)
    throw std::runtime_error("Failed to set default samplerate.");

  ret = mirisdr_set_tuner_gain_mode(_dev, int(!_auto_gain));
  if (ret < 0)
    throw std::runtime_error("Failed to enable manual gain mode.");
#endif
    sddc_stop_streaming(_dev);

  _buf = (unsigned short **) malloc(_buf_num * sizeof(unsigned short *));
  _buf_lens = (unsigned int *) malloc(_buf_num * sizeof(unsigned int));

  if (_buf && _buf_lens) {
    for(unsigned int i = 0; i < _buf_num; ++i)
      _buf[i] = (unsigned short *) malloc(BUF_SIZE);
  }
  sddc_set_async_params(_dev, 0, 0, ngrx_source_c::_sddc_callback, this);
  sddc_start_streaming(_dev);
}

/*
 * Our virtual destructor.
 */
ngrx_source_c::~ngrx_source_c ()
{
  if (_dev) {
    _running = false;
    sddc_stop_streaming(_dev);
    sddc_close( _dev );
    _dev = NULL;
  }

  if (_buf) {
    for(unsigned int i = 0; i < _buf_num; ++i) {
      free(_buf[i]);
    }

    free(_buf);
    _buf = NULL;
    free(_buf_lens);
    _buf_lens = NULL;
  }
}

void ngrx_source_c::_sddc_callback(uint32_t len, const float *buf, void *ctx)
{
  ngrx_source_c *obj = (ngrx_source_c *)ctx;
  obj->sddc_callback((unsigned char *)buf, len * 4);
}

void ngrx_source_c::sddc_callback(unsigned char *buf, uint32_t len)
{
  if (_skipped < BUF_SKIP) {
    _skipped++;
    return;
  }

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    if (len > BUF_SIZE)
    {
      std::cerr<<"len="<<len<<" BUF_SIZE="<<BUF_SIZE<<std::endl;
      throw std::runtime_error("Buffer too small.");
    }

    int buf_tail = (_buf_head + _buf_used) % _buf_num;
    memcpy(_buf[buf_tail], buf, len);
    _buf_lens[buf_tail] = len;

    if (_buf_used == _buf_num) {
      std::cerr << "O" << std::flush;
      _buf_head = (_buf_head + 1) % _buf_num;
    } else {
      _buf_used++;
    }
  }

  _buf_cond.notify_one();
}

void ngrx_source_c::rearm_dcr()
{
  return;
  _dc_size = sddc_get_sample_rate(_dev);
  _dc_loops = 0;
  _dc_count = 0;
  _dc_accum = gr_complex(0.0);
}

int ngrx_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];
  int processed = 0;

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while (_buf_used < 3 && _running) // collect at least 3 buffers
      _buf_cond.wait( lock );
  }

  if (!_running)
    return WORK_DONE;

  gr_complex *buf = (gr_complex *)_buf[_buf_head] + _buf_offset;

  if (noutput_items <= _samp_avail) {
    for (int i = 0; i < noutput_items; i++)
      *out++ = *(buf + i) - _dc_offset;

    _buf_offset += noutput_items;
    _samp_avail -= noutput_items;
    processed += noutput_items;
  } else {
    for (int i = 0; i < _samp_avail; i++)
      *out++ = *(buf + i) - _dc_offset;

    {
      boost::mutex::scoped_lock lock( _buf_mutex );

      _buf_head = (_buf_head + 1) % _buf_num;
      _buf_used--;
    }

    buf = (gr_complex *)_buf[_buf_head];

    int remaining = noutput_items - _samp_avail;
    processed += _samp_avail;
    if(remaining > (int)_buf_lens[_buf_head] / BYTES_PER_SAMPLE)
      remaining = _buf_lens[_buf_head] / BYTES_PER_SAMPLE;

    for (int i = 0; i < remaining; i++)
      *out++ = *(buf + i) - _dc_offset;

    _buf_offset = remaining;
    _samp_avail = (_buf_lens[_buf_head] / BYTES_PER_SAMPLE) - remaining;
    processed += remaining;
  }
  if(0)
  if(_dc_loops < DC_LOOPS)
  {
    out = (gr_complex *)output_items[0];
    gr_complex loffset = gr_complex(0.0);
    for(int k = 0; k < processed; k++, out++)
    {
        _dc_accum += *out - loffset;
        _dc_count++;
        if(_dc_count == _dc_size)
        {
            _dc_offset += _dc_accum / float(_dc_size);
            loffset += _dc_accum / float(_dc_size);
            _dc_accum = gr_complex(0.0);
            _dc_count = 0;
            _dc_loops ++;
            if(_dc_loops == DC_LOOPS)
                break;
        }
    }
  }
  return processed;
}

std::vector<std::string> ngrx_source_c::get_devices()
{
  std::vector<std::string> devices;

  for (int i = 0; i < sddc_get_device_count(); i++) {
    std::string args = "ngrx=" + boost::lexical_cast< std::string >( i );
    //args += ",label='" + std::string(mirisdr_get_device_name( i )) + "'";
    devices.push_back( args );
  }

  return devices;
}

size_t ngrx_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t ngrx_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  range += osmosdr::range_t( 2000000 );
  range += osmosdr::range_t( 4000000 );
  range += osmosdr::range_t( 8000000 );
  range += osmosdr::range_t( 16000000 );
  range += osmosdr::range_t( 32000000 );
  range += osmosdr::range_t( 64000000 );

  return range;
}

double ngrx_source_c::set_sample_rate(double rate)
{
  if (_dev) {
    sddc_set_sample_rate( _dev, (uint32_t)rate);
    rearm_dcr();
  }

  return get_sample_rate();
}

double ngrx_source_c::get_sample_rate()
{
  if (_dev)
    return (double)sddc_get_sample_rate( _dev );

  return 0;
}

osmosdr::freq_range_t ngrx_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  range += osmosdr::range_t( 0e3, 1730e6 ); /* LW/MW/SW (150 kHz - 30 MHz) */

  return range;
}

double ngrx_source_c::set_center_freq( double freq, size_t chan )
{
#define APPLY_PPM_CORR(val, ppm) ((val) * (1.0 + (ppm) * 0.000001))
  if (_dev)
  {
    _center_freq = freq;
    double corr_freq = APPLY_PPM_CORR( freq, _freq_corr );
    sddc_set_tuner_frequency( _dev, (uint32_t)corr_freq );
  }
  return get_center_freq( chan );
}

double ngrx_source_c::get_center_freq( size_t chan )
{
  if (_dev)
//      return double(mirisdr_get_center_freq( _dev )) / (1.0 + _freq_corr * 0.000001);
    return _center_freq;
  return 0;
}

double ngrx_source_c::set_freq_corr( double ppm, size_t chan )
{
  _freq_corr = ppm;
  set_center_freq( _center_freq );
  return get_freq_corr( chan );
}

double ngrx_source_c::get_freq_corr( size_t chan )
{
  return _freq_corr;
}

std::vector<std::string> ngrx_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > gains;

  gains += "IF";
  gains += "Tuner";

  return gains;
}

osmosdr::gain_range_t ngrx_source_c::get_gain_range( size_t chan )
{
  osmosdr::gain_range_t range;

  if (_dev) {
      return osmosdr::gain_range_t(3, 56, 1);
  }

  return range;
}

osmosdr::gain_range_t ngrx_source_c::get_gain_range( const std::string & name, size_t chan )
{
  osmosdr::gain_range_t range;
  if (_dev) {
    if(name == "IF")
        return osmosdr::gain_range_t(-20, 40, 1);
    if(name == "Tuner")
        return osmosdr::gain_range_t(-20, 40, 1);
   }
   return range;
}

bool ngrx_source_c::set_gain_mode( bool automatic, size_t chan )
{
  if (_dev) {
//    if (!mirisdr_set_tuner_gain_mode(_dev, int(!automatic))) {
//      _auto_gain = automatic;
//    }
  }

  return get_gain_mode(chan);
}

bool ngrx_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double ngrx_source_c::set_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t rf_gains = ngrx_source_c::get_gain_range( chan );

  if (_dev) {
//    sddc_set_tuner_gain( _dev, int(rf_gains.clip(gain)) );
   rearm_dcr();
  }

  return get_gain( chan );
}

double ngrx_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if (_dev) {
    if(name == "IF")
    {
        sddc_set_tuner_if_attenuation(_dev, gain);
        return sddc_get_tuner_if_attenuation(_dev);
    }
    if(name == "Tuner")
    {
        sddc_set_tuner_rf_attenuation(_dev, gain);
        return sddc_get_tuner_rf_attenuation(_dev);
    }
   }
  return 0;
}

double ngrx_source_c::get_gain( size_t chan )
{
  if ( _dev )
  {
  //  return ((double)sddc_get_tuner_gain( _dev )) / 10.0;
  }
  return 0;
}

double ngrx_source_c::get_gain( const std::string & name, size_t chan )
{
  return get_gain( chan );
}

std::vector< std::string > ngrx_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string ngrx_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string ngrx_source_c::get_antenna( size_t chan )
{
  return "RX";
}

double ngrx_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  if ( bandwidth == 0.0 ) /* bandwidth of 0 means automatic filter selection */
    bandwidth = sddc_get_sample_rate( _dev ) * 0.75; /* select narrower filters to prevent aliasing */

  if ( _dev ) {
    //mirisdr_set_bandwidth( _dev, uint32_t(bandwidth) );
    rearm_dcr();
    return get_bandwidth( chan);
  }

  return 0.0;
}

double ngrx_source_c::get_bandwidth( size_t chan )
{
  //return double(sddc_get_bandwidth( _dev ));
  return 0;
}
