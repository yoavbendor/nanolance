#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

#ifdef NANO_LANCE_READER_HAS_S3
#include <s3_aws_sdk_stream.h>
#endif

namespace {

void set_error(char* error_message, size_t error_message_capacity, const std::string& msg) {
    if (error_message != nullptr && error_message_capacity > 0U) {
        std::strncpy(error_message, msg.c_str(), error_message_capacity - 1U);
        error_message[error_message_capacity - 1U] = '\0';
    }
}

}  // namespace

extern "C" {

int nano_lance_fetch_external_blob(const char* uri, uint64_t position, uint64_t size, uint8_t* out_buf, size_t out_cap,
                                   size_t* bytes_read, char* error_message, size_t error_message_capacity) {
    if (uri == nullptr || uri[0] == '\0' || out_buf == nullptr || out_cap == 0U || bytes_read == nullptr) {
        set_error(error_message, error_message_capacity, "invalid argument");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    *bytes_read = 0U;
    const std::string suri(uri);
    if (suri.rfind("file://", 0) == 0) {
        const std::string path = suri.substr(7);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            set_error(error_message, error_message_capacity, "failed to open file:// path");
            return NANO_LANCE_READER_IO_ERROR;
        }
        if (position > 0U) {
            in.seekg(static_cast<std::streamoff>(position), std::ios::beg);
            if (!in) {
                set_error(error_message, error_message_capacity, "failed to seek file:// blob");
                return NANO_LANCE_READER_IO_ERROR;
            }
        }
        std::streamsize to_read = static_cast<std::streamsize>(out_cap);
        if (size != std::numeric_limits<uint64_t>::max()) {
            to_read = static_cast<std::streamsize>(std::min<uint64_t>(size, static_cast<uint64_t>(out_cap)));
        }
        in.read(reinterpret_cast<char*>(out_buf), to_read);
        *bytes_read = static_cast<size_t>(std::max<std::streamsize>(0, in.gcount()));
        return NANO_LANCE_READER_OK;
    }
#ifdef NANO_LANCE_READER_HAS_S3
    if (suri.rfind("s3://", 0) == 0) {
        static AwsSdkStreamFactory s3_factory;
        std::unique_ptr<std::istream> stream = s3_factory.open(suri, 32U * 1024U * 1024U);
        if (!stream) {
            set_error(error_message, error_message_capacity,
                      s3_factory.error().empty() ? "failed to open s3:// stream" : s3_factory.error());
            return NANO_LANCE_READER_IO_ERROR;
        }
        stream->seekg(static_cast<std::streamoff>(position), std::ios::beg);
        if (!*stream) {
            set_error(error_message, error_message_capacity, "failed to seek s3:// blob");
            return NANO_LANCE_READER_IO_ERROR;
        }
        std::streamsize to_read = static_cast<std::streamsize>(out_cap);
        if (size != std::numeric_limits<uint64_t>::max()) {
            to_read = static_cast<std::streamsize>(std::min<uint64_t>(size, static_cast<uint64_t>(out_cap)));
        }
        stream->read(reinterpret_cast<char*>(out_buf), to_read);
        *bytes_read = static_cast<size_t>(std::max<std::streamsize>(0, stream->gcount()));
        return NANO_LANCE_READER_OK;
    }
#endif
    (void)position;
    (void)size;
    set_error(error_message, error_message_capacity,
              "unsupported URI scheme (expected file://"
#ifdef NANO_LANCE_READER_HAS_S3
              " or s3://"
#endif
              ")");
    return NANO_LANCE_READER_IO_ERROR;
}

}  // extern "C"
