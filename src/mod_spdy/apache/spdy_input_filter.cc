// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mod_spdy/apache/spdy_input_filter.h"

#include <queue>

#include "mod_spdy/apache/http_stream_accumulator.h"
#include "mod_spdy/apache/input_filter_input_stream.h"
#include "mod_spdy/common/connection_context.h"
#include "mod_spdy/common/spdy_frame_pump.h"
#include "mod_spdy/common/spdy_stream_distributor.h"
#include "mod_spdy/common/spdy_to_http_converter.h"
#include "net/spdy/spdy_framer.h"

namespace mod_spdy {

// The SpdyToHttpConverterFactory creates SpdyToHttpConverter
// instances that write to an HttpStreamAccumulator. Each
// SpdyToHttpConverter gets its own dedicated HttpStreamAccumulator
// which is owned by the SpdyToHttpConverterFactory instance and
// placed in a queue. The SpdyToHttpConverterFactory also exposes a
// Read() method to drain the HttpStreamAccumulator instances. The
// HttpStreamAccumulators are drained in FIFO order. This is a
// sub-optimal implementation but it's the best we can do in a
// non-multiplexed environment. Because this is sub-optimal, we hide
// the class declaration inside this cc file instead of promoting it
// to a public header file.
class SpdyToHttpConverterFactory
    : public mod_spdy::SpdyFramerVisitorFactoryInterface {
 public:
  SpdyToHttpConverterFactory(spdy::SpdyFramer *framer,
                             apr_pool_t *pool,
                             apr_bucket_alloc_t *bucket_alloc);
  virtual ~SpdyToHttpConverterFactory();

  virtual spdy::SpdyFramerVisitorInterface *Create(
      spdy::SpdyStreamId stream_id);

  bool IsDataAvailable() const;

  bool HasError() const;

  // Read from the HttpStreamAccumulator queue. We read from the first
  // HttpStreamAccumulator in the queue, and do not begin reading from
  // the next HttpStreamAccumulator until the current
  // HttpStreamAccumulator is complete and empty.
  apr_status_t Read(apr_bucket_brigade *brigade,
                    ap_input_mode_t mode,
                    apr_read_type_e block,
                    apr_off_t readbytes);

 private:
  typedef std::queue<mod_spdy::HttpStreamAccumulator*> AccumulatorQueue;

  mod_spdy::HttpStreamAccumulator* GetAccumulator() const;

  // Modifies mutable member queue_
  void RemoveEmptyAccumulators() const;

  // We need to clean up the queue_ in some const methods, so we make
  // it mutable.
  mutable AccumulatorQueue queue_;
  spdy::SpdyFramer *const framer_;
  apr_pool_t *const pool_;
  apr_bucket_alloc_t *const bucket_alloc_;
};

SpdyToHttpConverterFactory::SpdyToHttpConverterFactory(
    spdy::SpdyFramer *framer, apr_pool_t *pool, apr_bucket_alloc_t *bucket_alloc)
    : framer_(framer), pool_(pool), bucket_alloc_(bucket_alloc) {
}

SpdyToHttpConverterFactory::~SpdyToHttpConverterFactory() {
  while (!queue_.empty()) {
    mod_spdy::HttpStreamAccumulator *accumulator = queue_.front();
    queue_.pop();
    delete accumulator;
  }
}

spdy::SpdyFramerVisitorInterface *SpdyToHttpConverterFactory::Create(
    spdy::SpdyStreamId stream_id) {
  mod_spdy::HttpStreamAccumulator *accumulator =
      new mod_spdy::HttpStreamAccumulator(pool_, bucket_alloc_);
  queue_.push(accumulator);
  return new mod_spdy::SpdyToHttpConverter(framer_, accumulator);
}

bool SpdyToHttpConverterFactory::IsDataAvailable() const {
  mod_spdy::HttpStreamAccumulator *accumulator = GetAccumulator();
  if (accumulator == NULL ||
      accumulator->HasError() ||
      accumulator->IsEmpty()) {
    return false;
  }
  return true;
}

bool SpdyToHttpConverterFactory::HasError() const {
  mod_spdy::HttpStreamAccumulator *accumulator = GetAccumulator();
  if (accumulator == NULL) {
    return false;
  }
  return accumulator->HasError();
}

apr_status_t SpdyToHttpConverterFactory::Read(apr_bucket_brigade *brigade,
                                              ap_input_mode_t mode,
                                              apr_read_type_e block,
                                              apr_off_t readbytes) {
  if (HasError()) {
    DCHECK(false);
    return APR_EGENERAL;
  }

  if (!IsDataAvailable()) {
    // TODO: return value needs to match what would be returned from
    // core_filters.c!
    return APR_SUCCESS;
  }

  mod_spdy::HttpStreamAccumulator *accumulator = GetAccumulator();
  apr_status_t rv = accumulator->Read(brigade, mode, block, readbytes);
  RemoveEmptyAccumulators();
  return rv;
}

// Not really const. Modifies mutable member queue_. This is necessary
// because we need to clean up the queue state from within const
// methods. As the internal state of each element in the queue
// changes, that element might become invalid and need to be removed
// from the queue (e.g. due to a call to
// HttpStreamAccumulator::OnTerminate()). We could have OnTerminate()
// synchronously publish an event to indicate that its state has
// changed which the queue manager could listen to, but that's overly
// complex. Instead we use the evil mutable keyword and hide this
// nastiness in the single method RemoveEmptyAccumulators(), which is
// marked const even though it changes the state of the mutable
// queue_.
void SpdyToHttpConverterFactory::RemoveEmptyAccumulators() const {
  while (!queue_.empty()) {
    mod_spdy::HttpStreamAccumulator *accumulator = queue_.front();
    if (accumulator->IsComplete() &&
        (accumulator->HasError() || accumulator->IsEmpty())) {
      queue_.pop();
      delete accumulator;
    } else {
      break;
    }
  }
}

mod_spdy::HttpStreamAccumulator*
SpdyToHttpConverterFactory::GetAccumulator() const {
  RemoveEmptyAccumulators();
  if (queue_.empty()) {
    return NULL;
  }
  return queue_.front();
}

SpdyInputFilter::SpdyInputFilter(conn_rec *c, ConnectionContext* context)
    : input_(new InputFilterInputStream(c->pool,
                                        c->bucket_alloc)),
      framer_(new spdy::SpdyFramer()),
      factory_(new SpdyToHttpConverterFactory(framer_.get(),
                                              c->pool,
                                              c->bucket_alloc)),
      distributor_(new mod_spdy::SpdyStreamDistributor(framer_.get(),
                                                       factory_.get())),
      pump_(new SpdyFramePump(input_.get(), framer_.get())),
      context_(context),
      temp_brigade_(apr_brigade_create(c->pool, c->bucket_alloc)) {
  framer_->set_visitor(distributor_.get());
}

SpdyInputFilter::~SpdyInputFilter() {
}

apr_status_t SpdyInputFilter::Read(ap_filter_t *filter,
                                   apr_bucket_brigade *brigade,
                                   ap_input_mode_t mode,
                                   apr_read_type_e block,
                                   apr_off_t readbytes) {
  if (context_->npn_state() == ConnectionContext::NOT_DONE_YET) {
    // NPN hasn't happened yet; we need to force some data through mod_ssl.  We
    // use a speculative read so that the data won't actually be consumed, and
    // will be returned again by the next read.
    apr_status_t rv = ap_get_brigade(filter->next, temp_brigade_,
                                     AP_MODE_SPECULATIVE, APR_BLOCK_READ, 1);
    apr_brigade_cleanup(temp_brigade_);
    // If the speculative read failed, NPN may not have happened yet.  Just
    // return the error code, and we'll try again next time.
    if (rv != APR_SUCCESS) {
      return rv;
    }
    // By this point, NPN should be done.  If our NPN callback still hasn't set
    // the NPN state to USING_SPDY or NOT_USING_SPDY, it's probably because
    // we're using a version of mod_ssl that doesn't support NPN.
    if (context_->npn_state() == ConnectionContext::NOT_DONE_YET) {
      LOG(WARNING) << "NPN never finished; does this mod_ssl support NPN?";
      context_->set_npn_state(ConnectionContext::NOT_USING_SPDY);
    }
  }

  // If we're not using SPDY, we should just forward the read onwards.
  if (context_->npn_state() != ConnectionContext::USING_SPDY) {
    // TODO(mdsteele): You'd think we should ap_remove_input_filter(filter) at
    //   this point, but things seem to break if we do.  It'd be nice to figure
    //   out why; maybe we're doing it wrong.
    return ap_get_brigade(filter->next, brigade, mode, block, readbytes);
  }

  if (filter->c->aborted) {
    // From mod_ssl's ssl_io_filter_input in ssl_engine_io.c
    apr_bucket *bucket = apr_bucket_eos_create(filter->c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);
    return APR_ECONNABORTED;
  }

  if (mode == AP_MODE_INIT) {
    // Nothing to do.
    return APR_SUCCESS;
  }

  input_->set_filter(filter, block);
  while (!factory_->HasError() && !factory_->IsDataAvailable()) {
    // If there's no data in the accumulator, attempt to pull more
    // data into it by driving the SpdyFramePump. Note that this will
    // not alway succeed; if there is no data available from the next
    // filter (e.g. no data to be read from the socket) then the
    // accumulator will not be populated with new data.
    if (!pump_->PumpOneFrame()) {
      break;
    }
  }
  input_->clear_filter();

  if (factory_->HasError()) {
    // TODO(bmcquade): how do we properly signal to the rest of apache
    // that we've encountered an error and the connection should be
    // closed?
    apr_bucket *bucket = apr_bucket_eos_create(filter->c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);
    return APR_EGENERAL;
  }

  apr_status_t rv = factory_->Read(brigade, mode, block, readbytes);
  if (rv == APR_SUCCESS && !factory_->IsDataAvailable()) {
    DCHECK(input_->IsEmpty());

    // If we've drained the internal buffers, then we should return
    // the status code we received the last time we read from the next
    // filter.
    return input_->next_filter_rv();
  }
  return rv;
}

}  // namespace mod_spdy
