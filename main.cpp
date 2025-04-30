#include <jack/jack.h>
#include <lo/lo.h>
#include <sndfile.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Constants
const char* CLIENT_NAME = "crossfade_client";
const float CROSSFADE_TIME = 0.5;  // Crossfade duration in seconds
const float MIN_DB = -60.0;

// Structure to hold audio file data
struct AudioFile {
  std::string filename;
  std::vector<float> buffer;
  sf_count_t length = 0;
  int channels = 0;
  int samplerate = 0;
};

// State variables
jack_client_t* client = nullptr;
jack_port_t* output_port = nullptr;
std::atomic<bool> is_crossfading(false);
std::atomic<bool> is_file_crossfading(false);
std::atomic<jack_nframes_t> current_position(0);
std::atomic<jack_nframes_t> target_position(0);
std::atomic<jack_nframes_t> crossfade_counter(0);
std::mutex audio_mutex;

// Audio data
std::unique_ptr<AudioFile> current_file = std::make_unique<AudioFile>();
std::unique_ptr<AudioFile> next_file = std::make_unique<AudioFile>();
int jack_sample_rate = 44100;

// Crossfade buffers
std::vector<float> fadeout_buffer;
std::vector<float> fadein_buffer;
size_t crossfade_length = 0;

// Crossfade curve (equal power crossfade)
float crossfade_curve(float ratio) {
  // Improved equal power crossfade curve with slight overlap
  return sinf((ratio * 0.5f + 0.25f) * M_PI);
}

// Fill crossfade buffers with audio data
void prepare_crossfade_buffers(bool is_file_switch) {
  size_t cf_length = CROSSFADE_TIME * jack_sample_rate;
  crossfade_length = cf_length;

  fadeout_buffer.resize(cf_length);
  fadein_buffer.resize(cf_length);

  jack_nframes_t curr_pos = current_position.load();
  jack_nframes_t targ_pos = target_position.load();

  // Fill fadeout buffer (from current position)
  for (size_t i = 0; i < cf_length; i++) {
    size_t pos = (curr_pos + i) % current_file->length;
    fadeout_buffer[i] = current_file->buffer[pos];
  }

  // Fill fadein buffer (from target position)
  if (is_file_switch) {
    // Crossfading between files
    for (size_t i = 0; i < cf_length; i++) {
      size_t pos = (targ_pos + i) % next_file->length;
      fadein_buffer[i] = next_file->buffer[pos];
    }
  } else {
    // Crossfading within the same file
    for (size_t i = 0; i < cf_length; i++) {
      size_t pos = (targ_pos + i) % current_file->length;
      fadein_buffer[i] = current_file->buffer[pos];
    }
  }
}

// Load audio file using libsndfile
bool load_audio_file(const char* filename, std::unique_ptr<AudioFile>& file) {
  SF_INFO sfinfo;
  memset(&sfinfo, 0, sizeof(sfinfo));

  SNDFILE* sndfile = sf_open(filename, SFM_READ, &sfinfo);
  if (!sndfile) {
    std::cerr << "Error opening audio file: " << filename << std::endl;
    std::cerr << "libsndfile error: " << sf_strerror(sndfile) << std::endl;
    return false;
  }

  file->filename = filename;
  file->length = sfinfo.frames;
  file->channels = sfinfo.channels;
  file->samplerate = sfinfo.samplerate;

  // Allocate buffer for audio data (mono conversion if needed)
  file->buffer.resize(file->length);

  // Read all frames
  if (file->channels == 1) {
    // Mono file, read directly
    sf_count_t count =
        sf_readf_float(sndfile, file->buffer.data(), file->length);
    if (count != file->length) {
      std::cerr << "Warning: Read fewer frames than expected: " << count
                << " vs " << file->length << std::endl;
    }
  } else {
    // Multi-channel file, mix down to mono
    std::vector<float> temp_buffer(file->length * file->channels);
    sf_count_t count =
        sf_readf_float(sndfile, temp_buffer.data(), file->length);

    // Mix down to mono
    for (sf_count_t i = 0; i < file->length; i++) {
      float sum = 0.0f;
      for (int c = 0; c < file->channels; c++) {
        sum += temp_buffer[i * file->channels + c];
      }
      file->buffer[i] = sum / file->channels;
    }
  }

  sf_close(sndfile);

  // Apply a slight normalization to prevent clipping
  float max_amp = 0.0f;
  for (size_t i = 0; i < file->length; i++) {
    max_amp = std::max(max_amp, std::abs(file->buffer[i]));
  }

  if (max_amp > 0.9f) {
    float gain = 0.9f / max_amp;
    for (size_t i = 0; i < file->length; i++) {
      file->buffer[i] *= gain;
    }
  }

  std::cout << "Loaded " << filename << ": " << file->length << " frames, "
            << file->channels << " channels, " << file->samplerate << " Hz"
            << std::endl;

  return true;
}

// Process callback function for JACK
int process_callback(jack_nframes_t nframes, void* arg) {
  jack_default_audio_sample_t* out =
      (jack_default_audio_sample_t*)jack_port_get_buffer(output_port, nframes);

  // Lock to prevent file changes during processing
  std::lock_guard<std::mutex> lock(audio_mutex);

  if (current_file->buffer.empty()) {
    // No audio loaded, output silence
    memset(out, 0, sizeof(jack_default_audio_sample_t) * nframes);
    return 0;
  }

  if (is_crossfading.load() || is_file_crossfading.load()) {
    // Crossfade mode
    jack_nframes_t fade_counter = crossfade_counter.load();

    for (jack_nframes_t i = 0; i < nframes; i++) {
      if (fade_counter < crossfade_length) {
        // Still crossfading
        float fade_ratio = static_cast<float>(fade_counter) / crossfade_length;

        // Apply crossfade curve (equal power)
        float fade_out_gain = cosf(fade_ratio * M_PI_2);
        float fade_in_gain = sinf(fade_ratio * M_PI_2);

        // Mix the samples with crossfade gains
        float mix = 0.0f;

        if (fade_counter < fadeout_buffer.size()) {
          mix += fadeout_buffer[fade_counter] * fade_out_gain;
        }

        if (fade_counter < fadein_buffer.size()) {
          mix += fadein_buffer[fade_counter] * fade_in_gain;
        }

        out[i] = mix;
        fade_counter++;
      } else {
        // Crossfade complete
        if (is_file_crossfading.load()) {
          // Complete file crossfade
          is_file_crossfading.store(false);

          // Swap files and set current position
          current_file.swap(next_file);
          next_file->buffer.clear();
          next_file->length = 0;

          // After crossfade, continue normal playback
          jack_nframes_t new_pos = (target_position.load() + crossfade_length) %
                                   current_file->length;
          current_position.store(new_pos);
        } else {
          // Complete within-file crossfade
          is_crossfading.store(false);

          // Set new position
          jack_nframes_t new_pos = (target_position.load() + crossfade_length) %
                                   current_file->length;
          current_position.store(new_pos);
        }

        // Output sample from current position
        jack_nframes_t pos = current_position.load();
        if (pos < current_file->length) {
          out[i] = current_file->buffer[pos];
          current_position.store((pos + 1) % current_file->length);
        } else {
          out[i] = 0.0f;
        }
      }
    }

    crossfade_counter.store(fade_counter);
  } else {
    // Normal playback
    for (jack_nframes_t i = 0; i < nframes; i++) {
      jack_nframes_t pos = current_position.load();
      if (pos < current_file->length) {
        out[i] = current_file->buffer[pos];
        current_position.store((pos + 1) % current_file->length);
      } else {
        out[i] = 0.0f;
      }
    }
  }

  return 0;
}

// JACK shutdown callback
void jack_shutdown(void* arg) {
  std::cerr << "JACK server shutdown, exiting..." << std::endl;
  exit(1);
}

// OSC message handler for seeking within current file
int osc_seek_handler(const char* path, const char* types, lo_arg** argv,
                     int argc, void* data, void* user_data) {
  if (argc < 1 || types[0] != 'f') {
    std::cerr << "Invalid OSC message format for /seek, expected float argument"
              << std::endl;
    return 1;
  }

  float seek_position_seconds = argv[0]->f;

  std::lock_guard<std::mutex> lock(audio_mutex);

  if (current_file->buffer.empty()) {
    std::cerr << "No audio loaded, cannot seek" << std::endl;
    return 1;
  }

  // Convert seconds to samples
  jack_nframes_t seek_position_samples =
      seek_position_seconds * current_file->samplerate;

  // Ensure seek position is within file bounds
  seek_position_samples =
      std::min(seek_position_samples, (jack_nframes_t)current_file->length - 1);

  std::cout << "Seeking to " << seek_position_seconds
            << " seconds in current file" << std::endl;

  // Set up crossfade
  if (is_crossfading.load() || is_file_crossfading.load()) {
    // Already crossfading, update target position
    target_position.store(seek_position_samples);
    crossfade_counter.store(0);
    is_crossfading.store(true);
    is_file_crossfading.store(false);

    // Prepare crossfade buffers
    prepare_crossfade_buffers(false);
  } else {
    // Start new crossfade
    target_position.store(seek_position_samples);
    crossfade_counter.store(0);
    is_crossfading.store(true);
    is_file_crossfading.store(false);

    // Prepare crossfade buffers
    prepare_crossfade_buffers(false);
  }

  return 0;
}

// OSC message handler for loading a new file
int osc_file_handler(const char* path, const char* types, lo_arg** argv,
                     int argc, void* data, void* user_data) {
  if (argc < 2 || types[0] != 's' || types[1] != 'f') {
    std::cerr << "Invalid OSC message format for /file, expected string and "
                 "float arguments"
              << std::endl;
    return 1;
  }

  std::string filename = &argv[0]->s;
  float seek_position_seconds = argv[1]->f;

  std::cout << "Loading file: " << filename << " and seeking to "
            << seek_position_seconds << " seconds" << std::endl;

  // Make a copy of the next_file pointer to avoid mutex deadlocks
  auto temp_file = std::make_unique<AudioFile>();

  // Load the new file
  if (!load_audio_file(filename.c_str(), temp_file)) {
    std::cerr << "Failed to load file: " << filename << std::endl;
    return 1;
  }

  // Convert seek position to samples
  jack_nframes_t seek_position_samples =
      seek_position_seconds * temp_file->samplerate;

  // Ensure seek position is within file bounds
  seek_position_samples =
      std::min(seek_position_samples, (jack_nframes_t)temp_file->length - 1);

  // Lock to update state
  {
    std::lock_guard<std::mutex> lock(audio_mutex);

    // If we don't have a current file, just set it directly
    if (current_file->buffer.empty()) {
      current_file = std::move(temp_file);
      current_position.store(seek_position_samples);
      return 0;
    }

    // Otherwise prepare for crossfade
    next_file = std::move(temp_file);
    target_position.store(seek_position_samples);
    crossfade_counter.store(0);
    is_crossfading.store(false);
    is_file_crossfading.store(true);

    // Prepare crossfade buffers
    prepare_crossfade_buffers(true);
  }

  return 0;
}

int main(int argc, char* argv[]) {
  // Initialize JACK client
  jack_status_t status;
  client = jack_client_open(CLIENT_NAME, JackNullOption, &status);

  if (client == nullptr) {
    std::cerr << "Failed to create JACK client" << std::endl;
    return 1;
  }

  // Register JACK callbacks
  jack_set_process_callback(client, process_callback, nullptr);
  jack_on_shutdown(client, jack_shutdown, nullptr);

  // Create output port
  output_port = jack_port_register(client, "output", JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsOutput, 0);

  if (output_port == nullptr) {
    std::cerr << "Failed to create JACK output port" << std::endl;
    jack_client_close(client);
    return 1;
  }

  // Get the sample rate from JACK
  jack_sample_rate = jack_get_sample_rate(client);
  std::cout << "JACK sample rate: " << jack_sample_rate << " Hz" << std::endl;

  // Calculate crossfade length
  crossfade_length = CROSSFADE_TIME * jack_sample_rate;

  // Initialize crossfade buffers
  fadeout_buffer.resize(crossfade_length);
  fadein_buffer.resize(crossfade_length);

  // Initialize OSC server
  lo_server_thread osc_server = lo_server_thread_new("7770", nullptr);

  if (osc_server == nullptr) {
    std::cerr << "Failed to create OSC server" << std::endl;
    jack_client_close(client);
    return 1;
  }

  // Add OSC message handlers
  lo_server_thread_add_method(osc_server, "/seek", "f", osc_seek_handler,
                              nullptr);
  lo_server_thread_add_method(osc_server, "/file", "sf", osc_file_handler,
                              nullptr);

  // Load a sample audio file if provided as command line argument
  if (argc > 1) {
    if (!load_audio_file(argv[1], current_file)) {
      std::cerr << "Failed to load audio file: " << argv[1] << std::endl;
      jack_client_close(client);
      lo_server_thread_free(osc_server);
      return 1;
    }
  }

  // Activate JACK client
  if (jack_activate(client) != 0) {
    std::cerr << "Failed to activate JACK client" << std::endl;
    jack_client_close(client);
    lo_server_thread_free(osc_server);
    return 1;
  }

  // Start OSC server
  lo_server_thread_start(osc_server);
  std::cout << "OSC server listening on port 7770" << std::endl;
  std::cout << "Available OSC commands:" << std::endl;
  std::cout << "  /seek <seconds> - Seek to position in current file"
            << std::endl;
  std::cout << "  /file <filename> <seconds> - Load file and seek to position"
            << std::endl;

  // Auto-connect to system output if possible
  const char** ports = jack_get_ports(client, nullptr, nullptr,
                                      JackPortIsPhysical | JackPortIsInput);
  if (ports != nullptr) {
    jack_connect(client, jack_port_name(output_port), ports[0]);
    if (ports[1] != nullptr) {
      // Connect to right channel if available
      jack_connect(client, jack_port_name(output_port), ports[1]);
    }
    jack_free(ports);
  }

  // Main loop
  std::cout << "Press Enter to quit..." << std::endl;
  std::cin.get();

  // Cleanup
  jack_deactivate(client);
  jack_client_close(client);
  lo_server_thread_free(osc_server);

  return 0;
}