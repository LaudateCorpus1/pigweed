// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_software_update/update_bundle_accessor.h"

#include <cstddef>
#include <cstring>
#include <string_view>

#include "pw_crypto/ecdsa.h"
#include "pw_crypto/sha256.h"
#include "pw_log/log.h"
#include "pw_protobuf/message.h"
#include "pw_result/result.h"
#include "pw_software_update/config.h"
#include "pw_software_update/manifest_accessor.h"
#include "pw_software_update/update_bundle.pwpb.h"
#include "pw_stream/interval_reader.h"
#include "pw_stream/memory_stream.h"

#define PW_LOG_LEVEL PW_SOFTWARE_UPDATE_CONFIG_LOG_LEVEL

namespace pw::software_update {
namespace {

Result<bool> VerifyEcdsaSignature(protobuf::Bytes public_key,
                                  ConstByteSpan digest,
                                  protobuf::Bytes signature) {
  // TODO(pwbug/456): Move this logic into an variant of the API in
  // pw_crypto:ecdsa that takes readers as inputs.
  std::byte public_key_bytes[65];
  std::byte signature_bytes[64];
  stream::IntervalReader key_reader = public_key.GetBytesReader();
  stream::IntervalReader sig_reader = signature.GetBytesReader();
  PW_TRY(key_reader.Read(public_key_bytes));
  PW_TRY(sig_reader.Read(signature_bytes));
  Status status = crypto::ecdsa::VerifyP256Signature(
      public_key_bytes, digest, signature_bytes);
  if (!status.ok()) {
    return false;
  }

  return true;
}

// Convert an integer from [0, 16) to a hex char
char IntToHex(uint8_t val) {
  PW_ASSERT(val < 16);
  return val >= 10 ? (val - 10) + 'a' : val + '0';
}

void LogKeyId(ConstByteSpan key_id) {
  char key_id_str[pw::crypto::sha256::kDigestSizeBytes * 2 + 1] = {0};
  for (size_t i = 0; i < pw::crypto::sha256::kDigestSizeBytes; i++) {
    uint8_t value = std::to_integer<uint8_t>(key_id[i]);
    key_id_str[i * 2] = IntToHex((value >> 4) & 0xf);
    key_id_str[i * 2 + 1] = IntToHex(value & 0xf);
  }

  PW_LOG_DEBUG("key_id: %s", key_id_str);
}

// Verifies signatures of a TUF metadata.
Status VerifyMetadataSignatures(protobuf::Bytes message,
                                protobuf::RepeatedMessages signatures,
                                protobuf::Message signature_requirement,
                                protobuf::StringToMessageMap key_mapping) {
  // Gets the threshold -- at least `threshold` number of signatures must
  // pass verification in order to trust this metadata.
  protobuf::Uint32 threshold = signature_requirement.AsUint32(
      static_cast<uint32_t>(SignatureRequirement::Fields::THRESHOLD));
  PW_TRY(threshold.status());

  // Gets the ids of keys that are allowed for verifying the signatures.
  protobuf::RepeatedBytes allowed_key_ids =
      signature_requirement.AsRepeatedBytes(
          static_cast<uint32_t>(SignatureRequirement::Fields::KEY_IDS));
  PW_TRY(allowed_key_ids.status());

  // Verifies the signatures. Check that at least `threshold` number of
  // signatures can be verified using the allowed keys.
  size_t verified_count = 0;
  size_t total_signatures = 0;
  for (protobuf::Message signature : signatures) {
    total_signatures++;
    protobuf::Bytes key_id =
        signature.AsBytes(static_cast<uint32_t>(Signature::Fields::KEY_ID));
    PW_TRY(key_id.status());

    // Reads the key id into a buffer, so that we can check whether it is
    // listed as allowed and look up the key value later.
    std::byte key_id_buf[pw::crypto::sha256::kDigestSizeBytes];
    stream::IntervalReader key_id_reader = key_id.GetBytesReader();
    Result<ByteSpan> key_id_read_res = key_id_reader.Read(key_id_buf);
    PW_TRY(key_id_read_res.status());
    if (key_id_read_res.value().size() != sizeof(key_id_buf)) {
      return Status::Internal();
    }

    // Verify that the `key_id` is listed in `allowed_key_ids`.
    // Note that the function assumes that the key id is properly derived
    // from the key (via sha256).
    bool key_id_is_allowed = false;
    for (protobuf::Bytes trusted : allowed_key_ids) {
      Result<bool> key_id_equal = trusted.Equal(key_id_buf);
      PW_TRY(key_id_equal.status());
      if (key_id_equal.value()) {
        key_id_is_allowed = true;
        break;
      }
    }

    if (!key_id_is_allowed) {
      PW_LOG_DEBUG("Skipping a key id not listed in allowed key ids.");
      LogKeyId(key_id_buf);
      continue;
    }

    // Retrieves the signature bytes.
    protobuf::Bytes sig =
        signature.AsBytes(static_cast<uint32_t>(Signature::Fields::SIG));
    PW_TRY(sig.status());

    // Extracts the key type, scheme and value information.
    std::string_view key_id_str(reinterpret_cast<const char*>(key_id_buf),
                                sizeof(key_id_buf));
    protobuf::Message key_info = key_mapping[key_id_str];
    PW_TRY(key_info.status());

    protobuf::Bytes key_val =
        key_info.AsBytes(static_cast<uint32_t>(Key::Fields::KEYVAL));
    PW_TRY(key_val.status());

    // The function assume that all keys are ECDSA keys. This is guaranteed
    // by the fact that all trusted roots have undergone content check.

    // computes the sha256 hash
    std::byte sha256_digest[32];
    stream::IntervalReader bytes_reader = message.GetBytesReader();
    PW_TRY(crypto::sha256::Hash(bytes_reader, sha256_digest));
    Result<bool> res = VerifyEcdsaSignature(key_val, sha256_digest, sig);
    PW_TRY(res.status());
    if (res.value()) {
      verified_count++;
      if (verified_count == threshold.value()) {
        return OkStatus();
      }
    }
  }

  if (total_signatures == 0) {
    // For self verification to tell apart unsigned bundles.
    return Status::NotFound();
  }

  PW_LOG_DEBUG(
      "Not enough number of signatures verified. Requires at least %u, "
      "verified %u",
      threshold.value(),
      verified_count);
  return Status::Unauthenticated();
}

// Verifies the signatures of a signed new root metadata against a given
// trusted root. The helper function extracts the corresponding key maping
// signature requirement, signatures from the trusted root and passes them
// to VerifyMetadataSignatures().
//
// Precondition: The trusted root metadata has undergone content validity check.
Result<bool> VerifyRootMetadataSignatures(protobuf::Message trusted_root,
                                          protobuf::Message new_root) {
  // Retrieves the trusted root metadata content message.
  protobuf::Message trusted = trusted_root.AsMessage(static_cast<uint32_t>(
      SignedRootMetadata::Fields::SERIALIZED_ROOT_METADATA));
  PW_TRY(trusted.status());

  // Retrieves the serialized new root metadata bytes.
  protobuf::Bytes serialized = new_root.AsBytes(static_cast<uint32_t>(
      SignedRootMetadata::Fields::SERIALIZED_ROOT_METADATA));
  PW_TRY(serialized.status());

  // Gets the key mapping from the trusted root metadata.
  protobuf::StringToMessageMap key_mapping = trusted.AsStringToMessageMap(
      static_cast<uint32_t>(RootMetadata::Fields::KEYS));
  PW_TRY(key_mapping.status());

  // Gets the signatures of the new root.
  protobuf::RepeatedMessages signatures = new_root.AsRepeatedMessages(
      static_cast<uint32_t>(SignedRootMetadata::Fields::SIGNATURES));
  PW_TRY(signatures.status());

  // Gets the signature requirement from the trusted root metadata.
  protobuf::Message signature_requirement = trusted.AsMessage(
      static_cast<uint32_t>(RootMetadata::Fields::ROOT_SIGNATURE_REQUIREMENT));
  PW_TRY(signature_requirement.status());

  // Verifies the signatures.
  PW_TRY(VerifyMetadataSignatures(
      serialized, signatures, signature_requirement, key_mapping));
  return true;
}

Result<uint32_t> GetMetadataVersion(protobuf::Message& metadata,
                                    uint32_t common_metatdata_field_number) {
  // message [Root|Targets]Metadata {
  //   ...
  //   CommonMetadata common_metadata = <field_number>;
  //   ...
  // }
  //
  // message CommonMetadata {
  //   ...
  //   uint32 version = <field_number>;
  //   ...
  // }
  protobuf::Message common_metadata =
      metadata.AsMessage(common_metatdata_field_number);
  PW_TRY(common_metadata.status());
  protobuf::Uint32 res = common_metadata.AsUint32(
      static_cast<uint32_t>(software_update::CommonMetadata::Fields::VERSION));
  PW_TRY(res.status());
  return res.value();
}

// Reads a protobuf::String into a buffer and returns a std::string_view.
Result<std::string_view> ReadProtoString(protobuf::String str,
                                         std::span<char> buffer) {
  stream::IntervalReader reader = str.GetBytesReader();
  if (reader.interval_size() > buffer.size()) {
    return Status::ResourceExhausted();
  }

  Result<ByteSpan> res = reader.Read(std::as_writable_bytes(buffer));
  PW_TRY(res.status());
  return std::string_view(buffer.data(), res.value().size());
}

}  // namespace

Status UpdateBundleAccessor::OpenAndVerify() {
  PW_TRY(DoOpen());

  if (Status status = DoVerify(); !status.ok()) {
    Close();
    return status;
  }

  return OkStatus();
}

Result<uint64_t> UpdateBundleAccessor::GetTotalPayloadSize() {
  protobuf::RepeatedMessages manifested_targets =
      GetManifest().GetTargetFiles();
  PW_TRY(manifested_targets.status());

  protobuf::StringToBytesMap bundled_payloads = bundle_.AsStringToBytesMap(
      static_cast<uint32_t>(UpdateBundle::Fields::TARGET_PAYLOADS));
  PW_TRY(bundled_payloads.status());

  uint64_t total_bytes;
  std::array<std::byte, MAX_TARGET_NAME_LENGTH> name_buffer = {};
  for (protobuf::Message target : manifested_targets) {
    protobuf::String target_name =
        target.AsString(static_cast<uint32_t>(TargetFile::Fields::FILE_NAME));

    stream::IntervalReader name_reader = target_name.GetBytesReader();
    PW_TRY(name_reader.status());
    if (name_reader.interval_size() > name_buffer.size()) {
      return Status::OutOfRange();
    }

    Result<ByteSpan> read_result = name_reader.Read(name_buffer);
    PW_TRY(read_result.status());

    ConstByteSpan name_span = read_result.value();
    std::string_view name_view(reinterpret_cast<const char*>(name_span.data()),
                               name_span.size_bytes());

    if (!bundled_payloads[name_view].ok()) {
      continue;
    }
    protobuf::Uint64 target_length =
        target.AsUint64(static_cast<uint32_t>(TargetFile::Fields::LENGTH));
    PW_TRY(target_length.status());
    total_bytes += target_length.value();
  }

  return total_bytes;
}

// Get the target element corresponding to `target_file`
stream::IntervalReader UpdateBundleAccessor::GetTargetPayload(
    std::string_view target_name) {
  protobuf::Message manifest_entry = GetManifest().GetTargetFile(target_name);
  PW_TRY(manifest_entry.status());

  protobuf::StringToBytesMap payloads_map = bundle_.AsStringToBytesMap(
      static_cast<uint32_t>(UpdateBundle::Fields::TARGET_PAYLOADS));
  return payloads_map[target_name].GetBytesReader();
}

// Get the target element corresponding to `target_file`
stream::IntervalReader UpdateBundleAccessor::GetTargetPayload(
    protobuf::String target_name) {
  char name_buf[MAX_TARGET_NAME_LENGTH] = {0};
  Result<std::string_view> name_view = ReadProtoString(target_name, name_buf);
  PW_TRY(name_view.status());
  return GetTargetPayload(name_view.value());
}

Status UpdateBundleAccessor::PersistManifest() {
  ManifestAccessor manifest = GetManifest();
  // GetManifest() fails if the bundle is yet to be verified.
  PW_TRY(manifest.status());

  // Notify backend to prepare to receive a new manifest.
  PW_TRY(backend_.BeforeManifestWrite());

  Result<stream::Writer*> writer = backend_.GetManifestWriter();
  PW_TRY(writer.status());
  PW_CHECK_NOTNULL(writer.value());

  PW_TRY(manifest.Export(*writer.value()));

  // Notify backend we are done writing. Backend should finalize
  // (seal the box).
  PW_TRY(backend_.AfterManifestWrite());

  return OkStatus();
}

Status UpdateBundleAccessor::Close() {
  bundle_verified_ = false;
  return blob_store_reader_.IsOpen() ? blob_store_reader_.Close() : OkStatus();
}

Status UpdateBundleAccessor::DoOpen() {
  PW_TRY(blob_store_.Init());
  PW_TRY(blob_store_reader_.Open());
  bundle_ = protobuf::Message(blob_store_reader_,
                              blob_store_reader_.ConservativeReadLimit());
  if (!bundle_.ok()) {
    blob_store_reader_.Close();
    return bundle_.status();
  }
  return OkStatus();
}

Status UpdateBundleAccessor::DoVerify() {
#if PW_SOFTWARE_UPDATE_DISABLE_BUNDLE_VERIFICATION
  PW_LOG_WARN("Update bundle verification is disabled.");
  bundle_verified_ = true;
  return OkStatus();
#else   // PW_SOFTWARE_UPDATE_DISABLE_BUNDLE_VERIFICATION
  bundle_verified_ = false;

  // Verify and upgrade the on-device trust to the incoming root metadata if
  // one is included.
  PW_TRY(UpgradeRoot());

  // TODO(pwbug/456): Verify the targets metadata against the current trusted
  // root.
  PW_TRY(VerifyTargetsMetadata());

  // TODO(pwbug/456): Investigate whether targets payload verification should
  // be performed here or deferred until a specific target is requested.
  PW_TRY(VerifyTargetsPayloads());

  // TODO(pwbug/456): Invoke the backend to do downstream verification of the
  // bundle (e.g. compatibility and manifest completeness checks).

  bundle_verified_ = true;
  return OkStatus();
#endif  // PW_SOFTWARE_UPDATE_DISABLE_BUNDLE_VERIFICATION
}

protobuf::Message UpdateBundleAccessor::GetOnDeviceTrustedRoot() {
  Result<stream::SeekableReader*> res = backend_.GetRootMetadataReader();
  PW_TRY(res.status());
  PW_CHECK_NOTNULL(res.value());
  // Seek to the beginning so that ConservativeReadLimit() returns the correct
  // value.
  PW_TRY(res.value()->Seek(0, stream::Stream::Whence::kBeginning));
  return protobuf::Message(*res.value(), res.value()->ConservativeReadLimit());
}

ManifestAccessor UpdateBundleAccessor::GetOnDeviceManifest() {
  // Notify backend to check if an on-device manifest exists and is valid and if
  // yes, prepare a ready-to-go reader.
  PW_TRY(backend_.BeforeManifestRead());

  Result<stream::SeekableReader*> manifest_reader =
      backend_.GetManifestReader();
  PW_TRY(manifest_reader.status());
  PW_CHECK_NOTNULL(manifest_reader.value());

  // In case `backend_.BeforeManifestRead()` forgot to reset the reader.
  PW_TRY(manifest_reader.value()->Seek(0, stream::Stream::Whence::kBeginning));

  return ManifestAccessor::FromManifest(
      protobuf::Message(*manifest_reader.value(),
                        manifest_reader.value()->ConservativeReadLimit()));
}

Status UpdateBundleAccessor::UpgradeRoot() {
  protobuf::Message new_root = bundle_.AsMessage(
      static_cast<uint32_t>(UpdateBundle::Fields::ROOT_METADATA));

  // Try self-verification even if verification is disabled by the caller. This
  // minimizes surprises when the caller do decide to turn on verification.
  bool self_verifying = disable_verification_;

  // Choose and cache the root metadata to trust.
  trusted_root_ = self_verifying ? new_root : GetOnDeviceTrustedRoot();

  if (!new_root.status().ok()) {
    // Don't bother upgrading if not found or invalid.
    PW_LOG_WARN("Incoming root metadata not found or invalid.");
    return OkStatus();
  }

  // A valid trust anchor is required onwards from here.
  PW_TRY(trusted_root_.status());

  // TODO(pwbug/456): Check whether the bundle contains a root metadata that
  // is different from the on-device trusted root.

  // Verify the signatures against the trusted root metadata.
  Result<bool> verify_res =
      VerifyRootMetadataSignatures(trusted_root_, new_root);
  PW_TRY(verify_res.status());
  if (!verify_res.value()) {
    PW_LOG_INFO("Fail to verify signatures against the current root");
    return Status::Unauthenticated();
  }

  // TODO(pwbug/456): Verifiy the content of the new root metadata, including:
  //    1) Check role magic field.
  //    2) Check signature requirement. Specifically, check that no key is
  //       reused across different roles and keys are unique in the same
  //       requirement.
  //    3) Check key mapping. Specifically, check that all keys are unique,
  //       ECDSA keys, and the key ids are exactly the SHA256 of `key type +
  //       key scheme + key value`.

  // Verify the signatures against the new root metadata.
  verify_res = VerifyRootMetadataSignatures(new_root, new_root);
  PW_TRY(verify_res.status());
  if (!verify_res.value()) {
    PW_LOG_INFO("Fail to verify signatures against the new root");
    return Status::Unauthenticated();
  }

  // TODO(pwbug/456): Check rollback.
  // Retrieves the trusted root metadata content message.
  protobuf::Message trusted_root_content =
      trusted_root_.AsMessage(static_cast<uint32_t>(
          SignedRootMetadata::Fields::SERIALIZED_ROOT_METADATA));
  PW_TRY(trusted_root_content.status());
  Result<uint32_t> trusted_root_version = GetMetadataVersion(
      trusted_root_content,
      static_cast<uint32_t>(RootMetadata::Fields::COMMON_METADATA));
  PW_TRY(trusted_root_version.status());

  // Retrieves the serialized new root metadata message.
  protobuf::Message new_root_content = new_root.AsMessage(static_cast<uint32_t>(
      SignedRootMetadata::Fields::SERIALIZED_ROOT_METADATA));
  PW_TRY(new_root_content.status());
  Result<uint32_t> new_root_version = GetMetadataVersion(
      new_root_content,
      static_cast<uint32_t>(RootMetadata::Fields::COMMON_METADATA));
  PW_TRY(new_root_version.status());

  if (trusted_root_version.value() > new_root_version.value()) {
    PW_LOG_DEBUG("Root attempts to rollback from %u to %u.",
                 trusted_root_version.value(),
                 new_root_version.value());
    return Status::Unauthenticated();
  }

  if (!self_verifying) {
    // Persist the root immediately after it is successfully verified. This is
    // to make sure the trust anchor is up-to-date in storage as soon as
    // we are confident. Although targets metadata and product-specific
    // verification have not been done yet. They should be independent from and
    // not gate the upgrade of root key. This allows timely revokation of
    // compromise keys.
    stream::IntervalReader new_root_reader =
        new_root.ToBytes().GetBytesReader();
    PW_TRY(backend_.SafelyPersistRootMetadata(new_root_reader));
  }

  // TODO(pwbug/456): Implement key change detection to determine whether
  // rotation has occured or not. Delete the persisted targets metadata version
  // if any of the targets keys has been rotated.

  return OkStatus();
}

Status UpdateBundleAccessor::VerifyTargetsMetadata() {
  bool self_verifying = disable_verification_;

  if (self_verifying && !trusted_root_.status().ok()) {
    PW_LOG_WARN(
        "Targets metadata self-verification is noop due to unavailable Root.");
    return OkStatus();
  }

  // A valid trust anchor is required from now on.
  PW_TRY(trusted_root_.status());

  // Retrieve the signed targets metadata map.
  //
  // message UpdateBundle {
  //   ...
  //   map<string, SignedTargetsMetadata> target_metadata = <id>;
  //   ...
  // }
  protobuf::StringToMessageMap signed_targets_metadata_map =
      bundle_.AsStringToMessageMap(
          static_cast<uint32_t>(UpdateBundle::Fields::TARGETS_METADATA));
  PW_TRY(signed_targets_metadata_map.status());

  // The top-level targets metadata is identified by key name "targets" in the
  // map.
  protobuf::Message signed_top_level_targets_metadata =
      signed_targets_metadata_map[kTopLevelTargetsName];
  PW_TRY(signed_top_level_targets_metadata.status());

  // Retrieve the serialized metadata.
  //
  // message SignedTargetsMetadata {
  //   ...
  //   bytes serialized_target_metadata = <id>;
  //   ...
  // }
  protobuf::Message top_level_targets_metadata =
      signed_top_level_targets_metadata.AsMessage(static_cast<uint32_t>(
          SignedTargetsMetadata::Fields::SERIALIZED_TARGETS_METADATA));

  // Get the sigantures from the signed targets metadata.
  protobuf::RepeatedMessages signatures =
      signed_top_level_targets_metadata.AsRepeatedMessages(
          static_cast<uint32_t>(SignedTargetsMetadata::Fields::SIGNATURES));
  PW_TRY(signatures.status());

  // Retrieve the trusted root metadata message.
  protobuf::Message trusted_root =
      trusted_root_.AsMessage(static_cast<uint32_t>(
          SignedRootMetadata::Fields::SERIALIZED_ROOT_METADATA));
  PW_TRY(trusted_root.status());

  // Get the key_mapping from the trusted root metadata.
  protobuf::StringToMessageMap key_mapping = trusted_root.AsStringToMessageMap(
      static_cast<uint32_t>(RootMetadata::Fields::KEYS));
  PW_TRY(key_mapping.status());

  // Get the targest metadtata siganture requirement from the trusted root.
  protobuf::Message signature_requirement =
      trusted_root.AsMessage(static_cast<uint32_t>(
          RootMetadata::Fields::TARGETS_SIGNATURE_REQUIREMENT));
  PW_TRY(signature_requirement.status());

  // Verify the sigantures
  Status sig_res =
      VerifyMetadataSignatures(top_level_targets_metadata.ToBytes(),
                               signatures,
                               signature_requirement,
                               key_mapping);

  if (self_verifying && sig_res.IsNotFound()) {
    PW_LOG_WARN("Unsigned bundles ignored by self-verification.");
    return OkStatus();
  }

  PW_TRY(sig_res);

  // TODO(pwbug/456): Check targets metadtata content.

  if (self_verifying) {
    // Don't bother because it does not matter.
    PW_LOG_WARN(
        "Self verification does not do Targets metadata anti-rollback.");
    return OkStatus();
  }

  // Anti-rollback check.
  ManifestAccessor device_manifest = GetOnDeviceManifest();
  if (device_manifest.status().IsNotFound()) {
    PW_LOG_WARN("Skipping OTA anti-rollback due to absent device manifest.");
    return OkStatus();
  }

  protobuf::Uint32 current_version = device_manifest.GetVersion();
  PW_TRY(current_version.status());

  // Retrieves the version from the new metadata
  Result<uint32_t> new_version = GetMetadataVersion(
      top_level_targets_metadata,
      static_cast<uint32_t>(
          software_update::TargetsMetadata::Fields::COMMON_METADATA));
  PW_TRY(new_version.status());
  if (current_version.value() > new_version.value()) {
    PW_LOG_DEBUG("Targets attempt to rollback from %u to %u.",
                 current_version.value(),
                 new_version.value());
    return Status::Unauthenticated();
  }

  return OkStatus();
}

Status UpdateBundleAccessor::VerifyTargetsPayloads() {
  ManifestAccessor bundle_manifest = ManifestAccessor::FromBundle(bundle_);
  PW_TRY(bundle_manifest.status());

  // Target file descriptors (pathname, length, hash, etc.) listed in the bundle
  // manifest.
  protobuf::RepeatedMessages target_files = bundle_manifest.GetTargetFiles();
  PW_TRY(target_files.status());

  // Verify length and SHA256 hash for each file listed in the manifest.
  for (protobuf::Message target_file : target_files) {
    // Extract target file name in the form of a `std::string_view`.
    protobuf::String name_proto = target_file.AsString(
        static_cast<uint32_t>(TargetFile::Fields::FILE_NAME));
    PW_TRY(name_proto.status());
    char name_buf[MAX_TARGET_NAME_LENGTH] = {0};
    Result<std::string_view> target_name =
        ReadProtoString(name_proto, name_buf);
    PW_TRY(target_name.status());

    // Get target length.
    protobuf::Uint64 target_length =
        target_file.AsUint64(static_cast<uint32_t>(TargetFile::Fields::LENGTH));
    PW_TRY(target_length.status());
    if (target_length.value() > PW_SOFTWARE_UPDATE_MAX_TARGET_PAYLOAD_SIZE) {
      PW_LOG_ERROR("Target payload too large. Maximum supported is %llu bytes.",
                   PW_SOFTWARE_UPDATE_MAX_TARGET_PAYLOAD_SIZE);
      return Status::OutOfRange();
    }

    // Get target SHA256 hash.
    protobuf::Bytes target_sha256 = Status::NotFound();
    protobuf::RepeatedMessages hashes = target_file.AsRepeatedMessages(
        static_cast<uint32_t>(TargetFile::Fields::HASHES));
    for (protobuf::Message hash : hashes) {
      protobuf::Uint32 hash_function =
          hash.AsUint32(static_cast<uint32_t>(Hash::Fields::FUNCTION));
      PW_TRY(hash_function.status());

      if (hash_function.value() ==
          static_cast<uint32_t>(HashFunction::SHA256)) {
        target_sha256 = hash.AsBytes(static_cast<uint32_t>(Hash::Fields::HASH));
        break;
      }
    }
    PW_TRY(target_sha256.status());

    PW_TRY(VerifyTargetPayload(
        bundle_manifest, target_name.value(), target_length, target_sha256));
  }  // for each target file in manifest.

  return OkStatus();
}

Status UpdateBundleAccessor::VerifyTargetPayload(
    ManifestAccessor manifest,
    std::string_view target_name,
    protobuf::Uint64 expected_length,
    protobuf::Bytes expected_sha256) {
  protobuf::StringToBytesMap payloads_map = bundle_.AsStringToBytesMap(
      static_cast<uint32_t>(UpdateBundle::Fields::TARGET_PAYLOADS));
  stream::IntervalReader payload_reader =
      payloads_map[target_name].GetBytesReader();

  Status status;
  if (payload_reader.ok()) {
    status = VerifyInBundleTargetPayload(
        expected_length, expected_sha256, payload_reader);
  } else {
    status = VerifyOutOfBundleTargetPayload(
        target_name, expected_length, expected_sha256);
  }

  // TODO(alizhang): Notify backend to do additional checks by calling
  // backend_.VerifyTargetFile(...).
  return status;
}

// TODO(alizhang): Add unit tests for all failure conditions.
Status UpdateBundleAccessor::VerifyOutOfBundleTargetPayload(
    std::string_view target_name,
    protobuf::Uint64 expected_length,
    protobuf::Bytes expected_sha256) {
#if PW_SOFTWARE_UPDATE_WITH_PERSONALIZATION
  // The target payload is "personalized out". We we can't take a measurement
  // without backend help. For now we will check against the device manifest
  // which contains a cached measurement of the last software update.
  ManifestAccessor device_manifest = GetOnDeviceManifest();
  if (!device_manifest.ok()) {
    PW_LOG_ERROR(
        "Can't verify personalized-out target because on-device manifest is "
        "not found.");
    return Status::Unauthenticated();
  }

  protobuf::Message cached = device_manifest.GetTargetFile(target_name);
  if (!cached.ok()) {
    PW_LOG_ERROR(
        "Can't verify personalized-out target because it is not found from "
        "on-device manifest.");
    return Status::Unauthenticated();
  }

  protobuf::Uint64 cached_length =
      cached.AsUint64(static_cast<uint32_t>(TargetFile::Fields::LENGTH));
  PW_TRY(cached_length.status());
  if (cached_length.value() != expected_length.value()) {
    PW_LOG_ERROR("Personalized-out target has bad length: %llu, expected: %llu",
                 cached_length.value(),
                 expected_length.value());
    return Status::Unauthenticated();
  }

  protobuf::Bytes cached_sha256 = Status::NotFound();
  protobuf::RepeatedMessages hashes = cached.AsRepeatedMessages(
      static_cast<uint32_t>(TargetFile::Fields::HASHES));
  for (protobuf::Message hash : hashes) {
    protobuf::Uint32 hash_function =
        hash.AsUint32(static_cast<uint32_t>(Hash::Fields::FUNCTION));
    PW_TRY(hash_function.status());

    if (hash_function.value() == static_cast<uint32_t>(HashFunction::SHA256)) {
      cached_sha256 = hash.AsBytes(static_cast<uint32_t>(Hash::Fields::HASH));
      break;
    }
  }
  std::byte sha256[crypto::sha256::kDigestSizeBytes] = {};
  PW_TRY(cached_sha256.GetBytesReader().Read(sha256));

  Result<bool> hash_equal = expected_sha256.Equal(sha256);
  PW_TRY(hash_equal.status());
  if (!hash_equal.value()) {
    PW_LOG_ERROR("Personalized-out target has a bad hash.");
    return Status::Unauthenticated();
  }

  return OkStatus();
#else
  PW_LOG_ERROR("Target file %s not found in bundle", target_name);
  return Status::Unauthenticated();
#endif  // PW_SOFTWARE_UPDATE_WITH_PERSONALIZATION
}

Status UpdateBundleAccessor::VerifyInBundleTargetPayload(
    protobuf::Uint64 expected_length,
    protobuf::Bytes expected_sha256,
    stream::IntervalReader payload_reader) {
  // If the target payload is included in the bundle, simply take a
  // measurement.
  uint64_t actual_length = payload_reader.interval_size();
  if (actual_length != expected_length.value()) {
    PW_LOG_ERROR("Wrong payload length. Expected: %llu, actual: %llu",
                 expected_length.value(),
                 actual_length);
    return Status::Unauthenticated();
  }

  std::byte actual_sha256[crypto::sha256::kDigestSizeBytes] = {};
  PW_TRY(crypto::sha256::Hash(payload_reader, actual_sha256));
  Result<bool> hash_equal = expected_sha256.Equal(actual_sha256);
  PW_TRY(hash_equal.status());
  if (!hash_equal.value()) {
    PW_LOG_ERROR("Wrong payload sha256 hash.");
    return Status::Unauthenticated();
  }

  return OkStatus();
}

ManifestAccessor UpdateBundleAccessor::GetManifest() {
  if (!bundle_verified_) {
    PW_LOG_DEBUG("Bundled has not passed verification yet");
    return Status::FailedPrecondition();
  }

  return ManifestAccessor::FromBundle(bundle_);
}

}  // namespace pw::software_update
