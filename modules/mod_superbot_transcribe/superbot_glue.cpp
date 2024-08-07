#include <cstdlib>
#include <algorithm>
#include <future>

#include <switch.h>
#include <switch_json.h>
#include <grpc++/grpc++.h>

#include "superbot/sbt_asr.grpc.pb.h"

namespace sbt_asr = superbot::asr;

#include "mod_superbot_transcribe.h"
#include "simple_buffer.h"

#define CHUNKSIZE (320)

namespace {
  int case_insensitive_match(std::string s1, std::string s2) {
   std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
   std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
   if(s1.compare(s2) == 0)
      return 1; //The strings are same
   return 0; //not matched
  }
}

class GStreamer {
public:
	GStreamer(
    switch_core_session_t *session, uint32_t channels, char* lang, char* dur_type, char* request_id, char* silence_duration) : 
      m_session(session), 
      m_writesDone(false), 
      m_connected(false), 
      m_language(lang),
      m_dur_type(dur_type),
      m_request_id(request_id),
      m_silence_duration(silence_duration),
      m_audioBuffer(CHUNKSIZE, 15) {
  
    const char* var;
    char sessionId[256];
    switch_channel_t *channel = switch_core_session_get_channel(session);
    strncpy(m_sessionId, switch_core_session_get_uuid(session), 256);
	}

	~GStreamer() {
		//switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_INFO, "GStreamer::~GStreamer - deleting channel and stub: %p\n", (void*)this);
	}

  void createInitMessage() {
    switch_channel_t *channel = switch_core_session_get_channel(m_session);

    const char* var = switch_channel_get_variable(channel, "SUPERBOT_ASR_URI");
    std::shared_ptr<grpc::Channel> grpcChannel = grpc::CreateChannel(var, grpc::InsecureChannelCredentials());
    if (!grpcChannel) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GStreamer %p failed creating grpc channel to %s\n", this, var);	
      throw std::runtime_error(std::string("Error creating grpc channel to ") + var);
    }

    m_stub = std::move(sbt_asr::SuperbotSpeechRecognition::NewStub(grpcChannel));

    /* set configuration parameters which are carried in the RecognitionInitMessage */
    auto streaming_config = m_request.mutable_streaming_config();

    auto config = streaming_config->mutable_config();
    config->set_sample_rate_hertz(8000);
    config->set_encoding(sbt_asr::AudioEncoding::LINEAR_PCM);
    config->set_audio_channel_count(1);

    /* language */
    config->set_language_code(m_language);

    config->set_dur_type(m_dur_type);
    config->set_request_id(m_request_id);
    config->set_silence_duration(m_silence_duration);

    /* model */
    if ((var = switch_channel_get_variable(channel, "SUPERBOT_MODEL"))) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "set model %s\n",var);
      config->set_model(var);
    }
  }

  void connect() {
    assert(!m_connected);
    // Begin a stream.

    createInitMessage();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p creating streamer\n", this);	
  	m_streamer = m_stub->StreamingRecognize(&m_context);
    m_connected = true;

    // read thread is waiting on this
    m_promise.set_value();

  	// Write the first request, containing the config only.
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p sending initial message\n", this);	
  	m_streamer->Write(m_request);
    m_request.clear_streaming_config();

    // send any buffered audio
    int nFrames = m_audioBuffer.getNumItems();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p got stream ready, %d buffered frames\n", this, nFrames);	
    if (nFrames) {
      char *p;
      do {
        p = m_audioBuffer.getNextChunk();
        if (p) {
          write(p, CHUNKSIZE);
        }
      } while (p);
    }
  }

	bool write(void* data, uint32_t datalen) {
    if (!m_connected) {
      if (datalen % CHUNKSIZE == 0) {
        m_audioBuffer.add(data, datalen);
      }
      return true;
    }
    m_request.clear_audio_content();
    m_request.set_audio_content(data, datalen);
    bool ok = m_streamer->Write(m_request);
    return ok;
  }

	uint32_t nextMessageSize(void) {
		uint32_t size = 0;
		m_streamer->NextMessageSize(&size);
		return size;
	}

	bool read(sbt_asr::StreamingRecognizeResponse* response) {
		return m_streamer->Read(response);
	}

	grpc::Status finish() {
		return m_streamer->Finish();
	}

  void startTimers() {
    //sbt_asr::StreamingRecognizeRequest request;
    //auto msg = request.mutable_control_message()->mutable_start_timers_message();
    //m_streamer->Write(request);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p sent start timers control message\n", this);	
  }

	void writesDone() {
    // grpc crashes if we call this twice on a stream
    if (!m_connected) {
      cancelConnect();
    }
    else if (!m_writesDone) {
      m_streamer->WritesDone();
      m_writesDone = true;
    }
	}

  bool waitForConnect() {
    std::shared_future<void> sf(m_promise.get_future());
    sf.wait();
    return m_connected;
  }

  void cancelConnect() {
    assert(!m_connected);
    m_promise.set_value();
  } 

  bool isConnected() {
    return m_connected;
  }

private:
	switch_core_session_t* m_session;
  grpc::ClientContext m_context;
	std::shared_ptr<grpc::Channel> m_channel;
	std::unique_ptr<sbt_asr::SuperbotSpeechRecognition::Stub> m_stub;
  sbt_asr::StreamingRecognizeRequest m_request;
	std::unique_ptr< grpc::ClientReaderWriterInterface<sbt_asr::StreamingRecognizeRequest, sbt_asr::StreamingRecognizeResponse> > m_streamer;
  bool m_writesDone;
  bool m_connected;
  std::string m_dur_type;
  std::string m_request_id;
  std::string m_silence_duration;
  std::string m_language;
  std::promise<void> m_promise;
  SimpleBuffer m_audioBuffer;
  char m_sessionId[256];
};

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
  static int count;
	struct cap_cb *cb = (struct cap_cb *) obj;
	GStreamer* streamer = (GStreamer *) cb->streamer;

  bool connected = streamer->waitForConnect();
  if (!connected) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "superbot transcribe grpc read thread exiting since we didnt connect\n") ;
    return nullptr;
  }

  // Read responses.
  sbt_asr::StreamingRecognizeResponse response;
  while (streamer->read(&response)) {  // Returns false when no more to read.
    count++;
    switch_core_session_t* session = switch_core_session_locate(cb->sessionId);
    if (!session) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "grpc_read_thread: session %s is gone!\n", cb->sessionId) ;
      return nullptr;
    }
    for (int r = 0; r < response.results_size(); ++r) {
      const auto& result = response.results(r);
      bool is_final = result.is_final();
      int num_alternatives = result.alternatives_size();
      int channel_tag = result.channel_tag();
      const char* lang = result.detected_lang().c_str();
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: detected lang\n", lang);

      cJSON * jResult = cJSON_CreateObject();
      cJSON * jIsFinal = cJSON_CreateBool(is_final);
      cJSON * jLang = cJSON_CreateString(lang);
      cJSON * jAlternatives = cJSON_CreateArray();
      cJSON * jChannelTag = cJSON_CreateNumber(channel_tag );
      cJSON_AddItemToObject(jResult, "alternatives", jAlternatives);
      cJSON_AddItemToObject(jResult, "is_final", jIsFinal);
      cJSON_AddItemToObject(jResult, "lang", jLang);

      for (int a = 0; a < num_alternatives; ++a) {
        cJSON* jAlt = cJSON_CreateObject();
        cJSON* jTranscript = cJSON_CreateString(result.alternatives(a).transcript().c_str());
        cJSON_AddItemToObject(jAlt, "transcript", jTranscript);

        auto confidence = result.alternatives(a).confidence();
        cJSON_AddNumberToObject(jAlt, "confidence", confidence);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "confidence %.2f\n", confidence) ;

        int words = result.alternatives(a).words_size();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "got %d words\n", words) ;
        if (words > 0) {
          cJSON* jWords = cJSON_CreateArray();
          for (int w = 0; w < words; w++) {
            cJSON* jWordInfo = cJSON_CreateObject();
            cJSON_AddItemToArray(jWords, jWordInfo);
            auto& wordInfo = result.alternatives(a).words(w);
            cJSON_AddStringToObject(jWordInfo, "word", wordInfo.word().c_str());
            cJSON_AddNumberToObject(jWordInfo, "confidence", wordInfo.confidence());
            cJSON_AddNumberToObject(jWordInfo, "speaker_tag", wordInfo.speaker_tag());
          }
          cJSON_AddItemToObject(jAlt, "words", jWords);
        }
        cJSON_AddItemToArray(jAlternatives, jAlt);
      }
      char* json = cJSON_PrintUnformatted(jResult);
      cb->responseHandler(session, (const char *) json, cb->bugname, NULL);
      free(json);

      cJSON_Delete(jResult);
    }
    switch_core_session_rwunlock(session);
  }
  return nullptr;
}
extern "C" {

    switch_status_t superbot_speech_init() {
      return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t superbot_speech_cleanup() {
      return SWITCH_STATUS_SUCCESS;
    }
    switch_status_t superbot_speech_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
      uint32_t samples_per_second, uint32_t channels, char* lang, char* dur_type, char* request_id, char* silence_duration, char *bugname, void **ppUserData) {

      switch_channel_t *channel = switch_core_session_get_channel(session);
      auto read_codec = switch_core_session_get_read_codec(session);
      uint32_t sampleRate = read_codec->implementation->actual_samples_per_second;
      struct cap_cb *cb;
      int err;

      cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
      strncpy(cb->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
      strncpy(cb->bugname, bugname, MAX_BUG_LEN);
      cb->end_of_utterance = 0;
      
      switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
      if (sampleRate != 8000) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_init:  initializing resampler\n");
          cb->resampler = speex_resampler_init(channels, sampleRate, 8000, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
           switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n",
                                 switch_channel_get_name(channel), speex_resampler_strerror(err));
          return SWITCH_STATUS_FALSE;
        }
      } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: no resampling needed for this call\n", switch_channel_get_name(channel));
      }
      cb->responseHandler = responseHandler;

      // allocate vad if we are delaying connecting to the recognizer until we detect speech
      if (switch_channel_var_true(channel, "START_RECOGNIZING_ON_VAD")) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_init:  initializing vad\n");
        cb->vad = switch_vad_init(sampleRate, channels);
        if (cb->vad) {
          const char* var;
          int mode = 2;
          int silence_ms = 150;
          int voice_ms = 250;
          int debug = 0;

          if (var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_MODE")) {
            mode = atoi(var);
          }
          if (var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_SILENCE_MS")) {
            silence_ms = atoi(var);
          }
          if (var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_VOICE_MS")) {
            voice_ms = atoi(var);
          }
          if (var = switch_channel_get_variable(channel, "RECOGNIZER_VAD_VOICE_MS")) {
            voice_ms = atoi(var);
          }
          switch_vad_set_mode(cb->vad, mode);
          switch_vad_set_param(cb->vad, "silence_ms", silence_ms);
          switch_vad_set_param(cb->vad, "voice_ms", voice_ms);
          switch_vad_set_param(cb->vad, "debug", debug);
        }
      }

      GStreamer *streamer = NULL;
      try {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_init:  allocating streamer\n");
        streamer = new GStreamer(session, channels, lang, dur_type, request_id, silence_duration);
        cb->streamer = streamer;
      } catch (std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing gstreamer: %s.\n", 
          switch_channel_get_name(channel), e.what());
        return SWITCH_STATUS_FALSE;
      }

      if (!cb->vad) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_init:  no vad so connecting to superbot immediately\n");
        streamer->connect();
      }

      // create the read thread
      switch_threadattr_t *thd_attr = NULL;
      switch_memory_pool_t *pool = switch_core_session_get_pool(session);

      switch_threadattr_create(&thd_attr, pool);
      switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
      switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

      *ppUserData = cb;
      return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t superbot_speech_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug) {
      switch_channel_t *channel = switch_core_session_get_channel(session);

      if (bug) {
        struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
        switch_mutex_lock(cb->mutex);

        if (!switch_channel_get_private(channel, cb->bugname)) {
          // race condition
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached (race).\n", switch_channel_get_name(channel));
          switch_mutex_unlock(cb->mutex);
          return SWITCH_STATUS_FALSE;
        }
        switch_channel_set_private(channel, cb->bugname, NULL);

        // close connection and get final responses
        GStreamer* streamer = (GStreamer *) cb->streamer;

        if (streamer) {
          streamer->writesDone();

          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_cleanup: GStreamer (%p) waiting for read thread to complete\n", (void*)streamer);
          switch_status_t st;
          switch_thread_join(&st, cb->thread);
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_cleanup:  GStreamer (%p) read thread completed\n", (void*)streamer);

          delete streamer;
          cb->streamer = NULL;
        }

        if (cb->resampler) {
          speex_resampler_destroy(cb->resampler);
        }
        if (cb->vad) {
          switch_vad_destroy(&cb->vad);
          cb->vad = nullptr;
        }
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }

			  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "superbot_speech_session_cleanup: Closed stream\n");

			  switch_mutex_unlock(cb->mutex);


			  return SWITCH_STATUS_SUCCESS;
      }

      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
      return SWITCH_STATUS_FALSE;
    }

    switch_bool_t superbot_speech_frame(switch_media_bug_t *bug, void* user_data) {
    	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    	struct cap_cb *cb = (struct cap_cb *) user_data;
		  if (cb->streamer && !cb->end_of_utterance) {
        GStreamer* streamer = (GStreamer *) cb->streamer;
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
          while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
            if (frame.datalen) {
              if (cb->vad && !streamer->isConnected()) {
                switch_vad_state_t state = switch_vad_process(cb->vad, (int16_t*) frame.data, frame.samples);
                if (state == SWITCH_VAD_STATE_START_TALKING) {
                  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "detected speech, connect to google speech now\n");
                  streamer->connect();
                  cb->responseHandler(session, "vad_detected", cb->bugname, NULL);
                }
              }

              if (cb->resampler) {
                spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
                spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
                spx_uint32_t in_len = frame.samples;
                size_t written;

                speex_resampler_process_interleaved_int(cb->resampler,
                  (const spx_int16_t *) frame.data,
                  (spx_uint32_t *) &in_len,
                  &out[0],
                  &out_len);
                streamer->write( &out[0], sizeof(spx_int16_t) * out_len);
              }
              else {
                streamer->write( frame.data, sizeof(spx_int16_t) * frame.samples);
              }
            }
          }
          switch_mutex_unlock(cb->mutex);
        }
      }
      return SWITCH_TRUE;
    }
}
