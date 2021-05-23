// Copyright 2021 Google LLC
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

#include "decoder_main_lib.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "cnpy.h"
#include "gilbert_model.h"
#include "glog/logging.h"
#include "include/ghc/filesystem.hpp"
#include "lyra_config.h"
#include "lyra_decoder.h"
#include "wav_util.h"

namespace chromemedia {
namespace codec {
namespace {

int PacketSize(LyraDecoder* decoder) {
  const float bits_per_packet = static_cast<float>(decoder->bitrate()) /
                                decoder->frame_rate() * kNumFramesPerPacket;
  const float bytes_per_packet = bits_per_packet / CHAR_BIT;
  return static_cast<int>(std::ceil(bytes_per_packet));
}

}  // namespace

bool DecodeFeatures(const std::vector<float>& features,
                    float packet_loss_rate, float average_burst_length,
                    LyraDecoder* decoder, std::vector<int16_t>* decoded_audio) {

  const int num_samples_per_packet =
      kNumFramesPerPacket * GetNumSamplesPerHop(decoder->sample_rate_hz());

  const auto benchmark_start = absl::Now();
  for (int encoded_index = 0; encoded_index < features.size();
       encoded_index += kNumFeatures) {
    const absl::Span<const float> encoded_features =
        absl::MakeConstSpan(features.data() + encoded_index, kNumFeatures);

    absl::optional<std::vector<int16_t>> decoded_or;
    if (!decoder->SetEncodedFeatures(encoded_features)) {
      LOG(ERROR) << "Unable to set encoded packet starting at byte "
                 << encoded_index;
      return false;
    }
    decoded_or = decoder->DecodeSamples(num_samples_per_packet);

    if (!decoded_or.has_value()) {
      LOG(ERROR) << "Unable to decode features starting at byte "
                 << encoded_index;
      return false;
    // } else if (encoded_index == 0) {
    //   std::cout << "Discarding the first frame" << std::endl;
    } else {
      decoded_audio->insert(decoded_audio->end(), decoded_or.value().begin(),
                            decoded_or.value().end());
    }
  }

  const auto elapsed = absl::Now() - benchmark_start;
  LOG(INFO) << "Elapsed seconds : " << absl::ToInt64Seconds(elapsed);
  LOG(INFO) << "Samples per second : "
            << decoded_audio->size() / absl::ToDoubleSeconds(elapsed);
  return true;
}

bool DecodeFile(const ghc::filesystem::path& encoded_path,
                const ghc::filesystem::path& output_path, int sample_rate_hz,
                float packet_loss_rate, float average_burst_length,
                const ghc::filesystem::path& model_path) {
  auto decoder =
      LyraDecoder::Create(sample_rate_hz, kNumChannels, kBitrate, model_path);
  if (decoder == nullptr) {
    LOG(ERROR) << "Could not create lyra decoder.";
    return false;
  }

  auto loaded_features = cnpy::npz_load(encoded_path, "features");
  std::cout << "Loaded features shape: ";
  for (const auto s : loaded_features.shape) {
    std::cout << s << ", ";
  }
  std::cout << std::endl;

  std::vector<int16_t> decoded_audio;
  if (!DecodeFeatures(loaded_features.as_vec<float>(), packet_loss_rate,
                      average_burst_length, decoder.get(), &decoded_audio)) {
    LOG(ERROR) << "Unable to decode features for file " << encoded_path;
    return false;
  }

  absl::Status write_status =
      Write16BitWavFileFromVector(output_path.string(), decoder->num_channels(),
                                  decoder->sample_rate_hz(), decoded_audio);
  if (!write_status.ok()) {
    LOG(ERROR) << write_status;
    return false;
  }
  return true;
}

}  // namespace codec
}  // namespace chromemedia
