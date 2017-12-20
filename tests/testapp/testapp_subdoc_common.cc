/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 *  Code common to various sub-document test cases.
 */

#include "testapp_subdoc_common.h"

std::ostream& operator<<(std::ostream& os, const BinprotSubdocCommand& obj) {
    os << "[cmd:" << memcached_opcode_2_text(obj.getOp())
       << " key:" << obj.getKey() << " path:" << obj.getPath()
       << " value:" << obj.getValue() << " flags:" << obj.getFlags()
       << " cas:" << obj.getCas() << "]";
    return os;
}

/* Encodes and sends a sub-document command, without waiting for any response.
 */
void send_subdoc_cmd(const BinprotSubdocCommand& cmd) {
    std::vector<uint8_t> buf;
    cmd.encode(buf);
    safe_send(buf.data(), buf.size(), false);
}

static void recv_subdoc_response(const BinprotSubdocCommand& cmd,
                                 protocol_binary_response_status err,
                                 BinprotSubdocResponse& resp) {
    std::vector<uint8_t> buf;
    if (!safe_recv_packet(buf)) {
        ADD_FAILURE() << "Failed to recv subdoc response";
        return;
    }

    protocol_binary_response_no_extras tempResponse;
    memcpy(&tempResponse, buf.data(), sizeof tempResponse);
    mcbp_validate_response_header(&tempResponse, cmd.getOp(), err);
    // safe_recv_packet does ntohl already!
    resp.assign(std::move(buf));
}

uint64_t recv_subdoc_response(protocol_binary_command expected_cmd,
                              protocol_binary_response_status expected_status,
                              const std::string& expected_value) {
    union {
        protocol_binary_response_subdocument response;
        char bytes[1024];
    } receive;

    if (!safe_recv_packet(receive.bytes, sizeof(receive.bytes))) {
        ADD_FAILURE() << "Failed to recv subdoc response";
        return -1;
    }

    mcbp_validate_response_header(
            (protocol_binary_response_no_extras*)&receive.response,
            expected_cmd,
            expected_status);

    const protocol_binary_response_header* header =
            &receive.response.message.header;

    const char* val_ptr =
            receive.bytes + sizeof(*header) + header->response.extlen;
    const size_t vallen = header->response.bodylen - header->response.extlen;

    if (!expected_value.empty() &&
        (expected_cmd != PROTOCOL_BINARY_CMD_SUBDOC_EXISTS)) {
        const std::string val(val_ptr, val_ptr + vallen);
        EXPECT_EQ(expected_value, val);
    } else {
        // Expect zero length on success (on error the error message string is
        // returned).
        if (header->response.status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            EXPECT_EQ(0u, vallen);
        }
    }
    return header->response.cas;
}

// Overload for multi-lookup responses
uint64_t recv_subdoc_response(
        protocol_binary_command expected_cmd,
        protocol_binary_response_status expected_status,
        const std::vector<SubdocMultiLookupResult>& expected_results) {
    union {
        protocol_binary_response_subdocument response;
        char bytes[1024];
    } receive;

    safe_recv_packet(receive.bytes, sizeof(receive.bytes));

    mcbp_validate_response_header(
            (protocol_binary_response_no_extras*)&receive.response,
            expected_cmd,
            expected_status);

    // Decode body and check against expected_results
    const auto& header = receive.response.message.header;
    const char* val_ptr =
            receive.bytes + sizeof(header) + header.response.extlen;
    const size_t vallen = header.response.bodylen + header.response.extlen;

    size_t offset = 0;
    for (unsigned int ii = 0; ii < expected_results.size(); ii++) {
        const size_t result_header_len = sizeof(uint16_t) + sizeof(uint32_t);
        if (offset + result_header_len > vallen) {
            ADD_FAILURE() << "Remaining value length too short for expected "
                             "result header";
            return -1;
        }

        const auto& exp_result = expected_results[ii];
        const char* result_header = val_ptr + offset;
        uint16_t status =
                ntohs(*reinterpret_cast<const uint16_t*>(result_header));
        EXPECT_EQ(exp_result.first, protocol_binary_response_status(status))
                << "Lookup result[" << ii << "]: status different";

        uint32_t result_len = ntohl(*reinterpret_cast<const uint32_t*>(
                result_header + sizeof(uint16_t)));
        EXPECT_EQ(exp_result.second.size(), result_len)
                << "Lookup result[" << ii << "]: length different";

        if (offset + result_header_len + result_len > vallen) {
            ADD_FAILURE() << "Remaining value length too short for expected "
                             "result value";
            return -1;
        }

        std::string result_value(result_header + result_header_len, result_len);
        EXPECT_EQ(exp_result.second, result_value) << "Lookup result[" << ii
                                                   << "]: value differs";

        offset += result_header_len + result_len;
    }

    return header.response.cas;
}

// Allow GTest to print out std::vectors as part of EXPECT/ ASSERT error
// messages.
namespace std {
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v) {
    os << '[';
    for (auto& e : v) {
        os << " " << e;
    }
    os << ']';
    return os;
}

// Specialization for uint8_t to print as hex.
template <>
std::ostream& operator<<(std::ostream& os, const std::vector<uint8_t>& v) {
    os << '[' << std::hex;
    for (auto& e : v) {
        os << " " << std::setw(2) << std::setfill('0') << (e & 0xff);
    }
    os << ']';
    return os;
}
} // namespace std

// Overload for multi-mutation responses
uint64_t recv_subdoc_response(
        protocol_binary_command expected_cmd,
        protocol_binary_response_status expected_status,
        const std::vector<SubdocMultiMutationResult>& expected_results) {
    union {
        protocol_binary_response_subdocument response;
        char bytes[1024];
    } receive;

    safe_recv_packet(receive.bytes, sizeof(receive.bytes));

    mcbp_validate_response_header(
            (protocol_binary_response_no_extras*)&receive.response,
            expected_cmd,
            expected_status);

    // TODO: Check extras for subdoc command and mutation / seqno (if enabled).

    // Decode body and check against expected_results
    const auto& header = receive.response.message.header;
    const char* val_ptr =
            receive.bytes + sizeof(header) + header.response.extlen;
    const size_t vallen = header.response.bodylen - header.response.extlen;
    std::string value(val_ptr, val_ptr + vallen);

    if (expected_status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        if (enabled_hello_features.count(cb::mcbp::Feature::MUTATION_SEQNO) >
            0) {
            EXPECT_EQ(16, header.response.extlen);
        } else {
            EXPECT_EQ(0u, header.response.extlen);
        }

        for (const auto& result : expected_results) {
            // Should always have at least 7 bytes in result -
            // index, status, resultlen.
            EXPECT_GE(value.size(),
                      sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t));

            // Extract fields from result spec and validate.
            uint8_t actual_index = *reinterpret_cast<uint8_t*>(&value[0]);
            value.erase(value.begin());
            EXPECT_EQ(result.index, actual_index);

            uint16_t actual_status =
                    ntohs(*reinterpret_cast<uint16_t*>(&value[0]));
            value.erase(value.begin(), value.begin() + 2);
            EXPECT_EQ(result.status, actual_status);

            uint32_t actual_resultlen =
                    ntohl(*reinterpret_cast<uint32_t*>(&value[0]));
            value.erase(value.begin(), value.begin() + 4);
            EXPECT_EQ(result.result.size(), actual_resultlen);

            std::string actual_result = value.substr(0, actual_resultlen);
            value.erase(value.begin(), value.begin() + actual_resultlen);
            EXPECT_EQ(result.result, actual_result);
        }
        // Should have consumed all of the value.
        EXPECT_EQ(0u, value.size());

    } else if (expected_status ==
               PROTOCOL_BINARY_RESPONSE_SUBDOC_MULTI_PATH_FAILURE) {
        // Specific path failed - should have a 3-byte body containing
        // specific status and index of first failing spec.
        EXPECT_EQ(3, vallen) << "Incorrect value:'"
                             << std::string(val_ptr, vallen) << '"';
        uint8_t actual_fail_index = *val_ptr;
        uint16_t actual_fail_spec_status =
                ntohs(*reinterpret_cast<const uint16_t*>(
                        val_ptr + sizeof(actual_fail_index)));
        EXPECT_EQ(1, expected_results.size());
        EXPECT_EQ(expected_results[0].index, actual_fail_index);
        EXPECT_EQ(expected_results[0].status, actual_fail_spec_status);
    } else {
        // Top-level error - should have zero body.
        EXPECT_EQ(0u, vallen);
    }

    return header.response.cas;
}

::testing::AssertionResult subdoc_verify_cmd(
        const BinprotSubdocCommand& cmd,
        protocol_binary_response_status err,
        const std::string& value,
        BinprotSubdocResponse& resp) {
    using ::testing::AssertionSuccess;
    using ::testing::AssertionFailure;

    send_subdoc_cmd(cmd);
    recv_subdoc_response(cmd, err, resp);
    if (::testing::Test::HasFailure()) {
        return AssertionFailure();
    }

    if (!value.empty() && cmd.getOp() != PROTOCOL_BINARY_CMD_SUBDOC_EXISTS) {
        if (value != resp.getValue()) {
            return AssertionFailure()
                   << "Value mismatch for " << cmd << std::endl
                   << "  Expected: " << value << std::endl
                   << "  Got: " << resp.getValue() << std::endl;
        }
    }
    return AssertionSuccess();
}

// Overload for multi-lookup commands.
uint64_t expect_subdoc_cmd(
        const SubdocMultiLookupCmd& cmd,
        protocol_binary_response_status expected_status,
        const std::vector<SubdocMultiLookupResult>& expected_results) {
    std::vector<char> payload = cmd.encode();
    safe_send(payload.data(), payload.size(), false);

    return recv_subdoc_response(PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP,
                                expected_status,
                                expected_results);
}

// Overload for multi-mutation commands.
uint64_t expect_subdoc_cmd(
        const SubdocMultiMutationCmd& cmd,
        protocol_binary_response_status expected_status,
        const std::vector<SubdocMultiMutationResult>& expected_results) {
    std::vector<char> payload = cmd.encode();
    safe_send(payload.data(), payload.size(), false);

    return recv_subdoc_response(cmd.command, expected_status, expected_results);
}

void store_object(const std::string& key,
                  const std::string& value,
                  bool compress) {
    const char* payload = value.c_str();
    size_t payload_len = value.size();
    char* deflated = NULL;
    if (compress) {
        payload_len = compress_document(payload, payload_len, &deflated);
        payload = deflated;
    }

    set_datatype_feature(true);
    store_object_w_datatype(key.c_str(), payload, payload_len, compress);
    set_datatype_feature(false);
    if (compress) {
        cb_free(deflated);
    }
}