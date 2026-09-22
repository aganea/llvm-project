//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declarations of the HTTPClient library for issuing
/// HTTP requests and handling the responses.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_HTTP_HTTPCLIENT_H
#define LLVM_HTTP_HTTPCLIENT_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <chrono>
#include <optional>

namespace llvm {

enum class HTTPMethod { GET };

/// A stateless description of an outbound HTTP request.
struct HTTPRequest {
  SmallString<128> Url;
  SmallVector<std::string, 0> Headers;
  HTTPMethod Method = HTTPMethod::GET;
  // Follow redirects without security downgrades.
  bool FollowRedirects = true;
  // Allow self-signed TLS certificates with this SHA-256 (WinHTTP only).
  std::optional<std::string> PinnedCertFingerprint;
  HTTPRequest(StringRef Url);
};

bool operator==(const HTTPRequest &A, const HTTPRequest &B);

/// A handler for state updates occurring while an HTTPRequest is performed.
/// Can trigger the client to abort the request by returning an Error from any
/// of its methods.
class HTTPResponseHandler {
public:
  /// Processes an additional chunk of bytes of the HTTP response body.
  virtual Error handleBodyChunk(StringRef BodyChunk) = 0;

protected:
  ~HTTPResponseHandler();
};

/// A reusable client that can perform HTTPRequests through a network socket.
class HTTPClient {
#if defined(LLVM_ENABLE_CURL) || defined(_WIN32)
  void *Handle = nullptr;
#endif

public:
  HTTPClient();
  ~HTTPClient();

  /// Legacy, non-atomic initialization snapshot. This field is not safe to
  /// read concurrently with initialize() or cleanup(). New code should use
  /// isInitialized(), which is safe to query concurrently with initialize().
  static bool IsInitialized;

  /// Returns whether the process-wide HTTP service is initialized.
  static bool isInitialized();

  /// Returns true only if LLVM has been compiled with a working HTTPClient.
  static bool isAvailable();

  /// Returns whether initialize() may first be called after other threads have
  /// started. This is false for libcurl versions which do not advertise
  /// thread-safe global initialization. Query this during controlled process
  /// startup, before allowing unrelated users to enter libcurl.
  static bool isInitializationThreadSafe();

  /// Initializes the process-wide HTTP service. Calls through LLVM are
  /// serialized and idempotent. When isInitializationThreadSafe() is false,
  /// the first call must still occur before the host starts other threads.
  static void initialize();

  /// Must be called only after all HTTP clients and requests have finished.
  /// It does not synchronize with live clients.
  static void cleanup();

  /// Sets the timeout for the entire request, in milliseconds. A zero or
  /// negative value means the request never times out.
  void setTimeout(std::chrono::milliseconds Timeout);

  /// Performs the Request, passing response data to the Handler. Returns all
  /// errors which occur during the request. Aborts if an error is returned by a
  /// Handler method.
  Error perform(const HTTPRequest &Request, HTTPResponseHandler &Handler);

  /// Returns the last received response code or zero if none.
  unsigned responseCode();
};

} // end namespace llvm

#endif // LLVM_HTTP_HTTPCLIENT_H
