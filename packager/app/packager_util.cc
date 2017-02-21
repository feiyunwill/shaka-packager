// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/app/packager_util.h"

#include <gflags/gflags.h>
#include <iostream>

#include "packager/app/fixed_key_encryption_flags.h"
#include "packager/app/playready_key_encryption_flags.h"
#include "packager/app/mpd_flags.h"
#include "packager/app/muxer_flags.h"
#include "packager/app/widevine_encryption_flags.h"
#include "packager/base/logging.h"
#include "packager/base/strings/string_number_conversions.h"
#include "packager/media/base/fixed_key_source.h"
#include "packager/media/base/muxer_options.h"
#include "packager/media/base/playready_key_source.h"
#include "packager/media/base/request_signer.h"
#include "packager/media/base/widevine_key_source.h"
#include "packager/media/file/file.h"
#include "packager/mpd/base/mpd_options.h"

DEFINE_bool(mp4_use_decoding_timestamp_in_timeline,
            false,
            "If set, decoding timestamp instead of presentation timestamp will "
            "be used when generating media timeline, e.g. timestamps in sidx "
            "and mpd. This is to workaround a Chromium bug that decoding "
            "timestamp is used in buffered range, https://crbug.com/398130.");
DEFINE_bool(dump_stream_info, false, "Dump demuxed stream info.");

namespace shaka {
namespace media {

std::unique_ptr<RequestSigner> CreateSigner() {
  std::unique_ptr<RequestSigner> signer;

  if (!FLAGS_aes_signing_key.empty()) {
    signer.reset(AesRequestSigner::CreateSigner(
        FLAGS_signer, FLAGS_aes_signing_key, FLAGS_aes_signing_iv));
    if (!signer) {
      LOG(ERROR) << "Cannot create an AES signer object from '"
                 << FLAGS_aes_signing_key << "':'" << FLAGS_aes_signing_iv
                 << "'.";
      return std::unique_ptr<RequestSigner>();
    }
  } else if (!FLAGS_rsa_signing_key_path.empty()) {
    std::string rsa_private_key;
    if (!File::ReadFileToString(FLAGS_rsa_signing_key_path.c_str(),
                                &rsa_private_key)) {
      LOG(ERROR) << "Failed to read from '" << FLAGS_rsa_signing_key_path
                 << "'.";
      return std::unique_ptr<RequestSigner>();
    }
    signer.reset(RsaRequestSigner::CreateSigner(FLAGS_signer, rsa_private_key));
    if (!signer) {
      LOG(ERROR) << "Cannot create a RSA signer object from '"
                 << FLAGS_rsa_signing_key_path << "'.";
      return std::unique_ptr<RequestSigner>();
    }
  }
  return signer;
}

std::unique_ptr<KeySource> CreateEncryptionKeySource() {
  std::unique_ptr<KeySource> encryption_key_source;
  if (FLAGS_enable_widevine_encryption) {
    std::unique_ptr<WidevineKeySource> widevine_key_source(
        new WidevineKeySource(FLAGS_key_server_url, FLAGS_include_common_pssh));
    if (!FLAGS_signer.empty()) {
      std::unique_ptr<RequestSigner> request_signer(CreateSigner());
      if (!request_signer)
        return std::unique_ptr<KeySource>();
      widevine_key_source->set_signer(std::move(request_signer));
    }

    std::vector<uint8_t> content_id;
    if (!base::HexStringToBytes(FLAGS_content_id, &content_id)) {
      LOG(ERROR) << "Invalid content_id hex string specified.";
      return std::unique_ptr<KeySource>();
    }
    Status status = widevine_key_source->FetchKeys(content_id, FLAGS_policy);
    if (!status.ok()) {
      LOG(ERROR) << "Widevine encryption key source failed to fetch keys: "
                 << status.ToString();
      return std::unique_ptr<KeySource>();
    }
    encryption_key_source = std::move(widevine_key_source);
  } else if (FLAGS_enable_fixed_key_encryption) {
    encryption_key_source = FixedKeySource::CreateFromHexStrings(
        FLAGS_key_id, FLAGS_key, FLAGS_pssh, FLAGS_iv);
  } else if (FLAGS_enable_playready_encryption) {
    if (!FLAGS_playready_key_id.empty() && !FLAGS_playready_key.empty()) {
      encryption_key_source = PlayReadyKeySource::CreateFromKeyAndKeyId(
          FLAGS_playready_key_id, FLAGS_playready_key);
    } else if (!FLAGS_playready_server_url.empty() &&
               !FLAGS_program_identifier.empty()) {
      std::unique_ptr<PlayReadyKeySource> playready_key_source;
      if (!FLAGS_client_cert_file.empty() &&
          !FLAGS_client_cert_private_key_file.empty() &&
          !FLAGS_client_cert_private_key_password.empty()) {
        playready_key_source.reset(new PlayReadyKeySource(
            FLAGS_playready_server_url,
            FLAGS_client_cert_file,
            FLAGS_client_cert_private_key_file,
            FLAGS_client_cert_private_key_password));
      } else {
        playready_key_source.reset(new PlayReadyKeySource(
            FLAGS_playready_server_url));
      }
      if (!FLAGS_ca_file.empty()) {
        playready_key_source->SetCaFile(FLAGS_ca_file);
      }
      playready_key_source->FetchKeysWithProgramIdentifier(FLAGS_program_identifier);
      encryption_key_source = std::move(playready_key_source);
    } else {
      LOG(ERROR) << "Error creating PlayReady key source.";
      return std::unique_ptr<KeySource>();
    }
  }
  return encryption_key_source;
}

std::unique_ptr<KeySource> CreateDecryptionKeySource() {
  std::unique_ptr<KeySource> decryption_key_source;
  if (FLAGS_enable_widevine_decryption) {
    std::unique_ptr<WidevineKeySource> widevine_key_source(
        new WidevineKeySource(FLAGS_key_server_url, FLAGS_include_common_pssh));
    if (!FLAGS_signer.empty()) {
      std::unique_ptr<RequestSigner> request_signer(CreateSigner());
      if (!request_signer)
        return std::unique_ptr<KeySource>();
      widevine_key_source->set_signer(std::move(request_signer));
    }

    decryption_key_source = std::move(widevine_key_source);
  } else if (FLAGS_enable_fixed_key_decryption) {
    const char kNoPssh[] = "";
    const char kNoIv[] = "";
    decryption_key_source = FixedKeySource::CreateFromHexStrings(
        FLAGS_key_id, FLAGS_key, kNoPssh, kNoIv);
  }
  return decryption_key_source;
}

bool GetMuxerOptions(MuxerOptions* muxer_options) {
  DCHECK(muxer_options);

  muxer_options->segment_duration = FLAGS_segment_duration;
  muxer_options->fragment_duration = FLAGS_fragment_duration;
  muxer_options->segment_sap_aligned = FLAGS_segment_sap_aligned;
  muxer_options->fragment_sap_aligned = FLAGS_fragment_sap_aligned;
  muxer_options->num_subsegments_per_sidx = FLAGS_num_subsegments_per_sidx;
  muxer_options->webm_subsample_encryption = FLAGS_webm_subsample_encryption;
  if (FLAGS_mp4_use_decoding_timestamp_in_timeline) {
    LOG(WARNING) << "Flag --mp4_use_decoding_timestamp_in_timeline is set. "
                    "Note that it is a temporary hack to workaround Chromium "
                    "bug https://crbug.com/398130. The flag may be removed "
                    "when the Chromium bug is fixed.";
  }
  muxer_options->mp4_use_decoding_timestamp_in_timeline =
      FLAGS_mp4_use_decoding_timestamp_in_timeline;

  muxer_options->temp_dir = FLAGS_temp_dir;
  return true;
}

bool GetMpdOptions(bool on_demand_profile, MpdOptions* mpd_options) {
  DCHECK(mpd_options);

  mpd_options->dash_profile =
      on_demand_profile ? DashProfile::kOnDemand : DashProfile::kLive;
  mpd_options->mpd_type =
      (on_demand_profile || FLAGS_generate_static_mpd)
          ? MpdType::kStatic
          : MpdType::kDynamic;
  mpd_options->availability_time_offset = FLAGS_availability_time_offset;
  mpd_options->minimum_update_period = FLAGS_minimum_update_period;
  mpd_options->min_buffer_time = FLAGS_min_buffer_time;
  mpd_options->time_shift_buffer_depth = FLAGS_time_shift_buffer_depth;
  mpd_options->suggested_presentation_delay =
      FLAGS_suggested_presentation_delay;
  mpd_options->default_language = FLAGS_default_language;
  return true;
}

}  // namespace media
}  // namespace shaka
