/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#include "config.h"
#include "user.h"

#include <platform/base64.h>
#include <platform/random.h>
#include <atomic>
#include <gsl/gsl>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace cb {
namespace sasl {
namespace pwdb {

std::atomic<int> IterationCount(4096);

class ScamShaFallbackSalt {
public:
    ScamShaFallbackSalt();

    void set(const std::string& salt) {
        std::lock_guard<std::mutex> guard(mutex);
        data = cb::base64::decode(salt);
    }

    std::vector<uint8_t> get() {
        std::lock_guard<std::mutex> guard(mutex);
        return std::vector<uint8_t>{data};
    }

protected:
    mutable std::mutex mutex;
    std::vector<uint8_t> data;
} scramsha_fallback_salt;

/**
 * Generate a salt and store it base64 encoded into the salt
 */
static void generateSalt(std::vector<uint8_t>& bytes, std::string& salt) {
    Couchbase::RandomGenerator randomGenerator(true);

    if (!randomGenerator.getBytes(bytes.data(), bytes.size())) {
        throw std::runtime_error("Failed to get random bytes");
    }

    using Couchbase::Base64::encode;

    salt = encode(
            std::string(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}

ScamShaFallbackSalt::ScamShaFallbackSalt()
    : data(cb::crypto::SHA512_DIGEST_SIZE) {
    std::string ignore;
    generateSalt(data, ignore);
}

User UserFactory::create(const std::string& unm, const std::string& passwd) {
    User ret{unm, false};

    struct {
        cb::crypto::Algorithm algoritm;
        Mechanism mech;
    } algo_info[] = {{cb::crypto::Algorithm::SHA1, Mechanism::SCRAM_SHA1},
                     {cb::crypto::Algorithm::SHA256, Mechanism::SCRAM_SHA256},
                     {cb::crypto::Algorithm::SHA512, Mechanism::SCRAM_SHA512}};

    // The format of the plain password encoding is that we're appending the
    // generated hmac to the salt (which should be 16 bytes). This makes
    // our plain text password generation compatible with ns_server
    std::vector<uint8_t> pwentry(16);
    std::string saltstring;
    generateSalt(pwentry, saltstring);
    std::vector<uint8_t> pw;
    std::copy(passwd.begin(), passwd.end(), std::back_inserter(pw));

    const auto hmac = cb::crypto::HMAC(
            cb::crypto::Algorithm::SHA1,
            {reinterpret_cast<const char*>(pwentry.data()), pwentry.size()},
            {reinterpret_cast<const char*>(pw.data()), pw.size()});
    std::copy(hmac.begin(), hmac.end(), std::back_inserter(pwentry));
    std::string hash{(const char*)pwentry.data(), pwentry.size()};

    ret.password[Mechanism::PLAIN] = User::PasswordMetaData{hash};

    for (const auto& info : algo_info) {
        if (cb::crypto::isSupported(info.algoritm)) {
            ret.generateSecrets(info.mech, passwd);
        }
    }

    return ret;
}

User UserFactory::createDummy(const std::string& unm, const Mechanism& mech) {
    User ret{unm};

    // Generate a random password
    std::vector<uint8_t> salt;
    std::string passwd;

    switch (mech) {
    case Mechanism::SCRAM_SHA512:
        salt.resize(cb::crypto::SHA512_DIGEST_SIZE);
        break;
    case Mechanism::SCRAM_SHA256:
        salt.resize(cb::crypto::SHA256_DIGEST_SIZE);
        break;
    case Mechanism::SCRAM_SHA1:
        salt.resize(cb::crypto::SHA1_DIGEST_SIZE);
        break;
    case Mechanism::PLAIN:
        throw std::logic_error(
                "cb::cbsasl::UserFactory::createDummy invalid algorithm");
    }

    if (salt.empty()) {
        throw std::logic_error(
                "cb::cbsasl::UserFactory::createDummy invalid algorithm");
    }

    generateSalt(salt, passwd);

    // Generate the secrets by using that random password
    ret.generateSecrets(mech, passwd);

    return ret;
}

User UserFactory::create(const cJSON* obj) {
    if (obj == nullptr) {
        throw std::runtime_error(
                "cb::cbsasl::UserFactory::create: obj cannot be null");
    }

    if (obj->type != cJSON_Object) {
        throw std::runtime_error(
                "cb::cbsasl::UserFactory::create: Invalid object type");
    }

    auto* o = cJSON_GetObjectItem(const_cast<cJSON*>(obj), "n");
    if (o == nullptr) {
        throw std::runtime_error(
                "cb::cbsasl::UserFactory::create: missing mandatory label 'n'");
    }
    if (o->type != cJSON_String) {
        throw std::runtime_error(
                "cb::cbsasl::UserFactory::create: 'n' must be a string");
    }

    User ret{o->valuestring, false};

    for (o = obj->child; o != nullptr; o = o->next) {
        std::string label(o->string);
        if (label == "n") {
            // skip. we've already processed this
        } else if (label == "sha512") {
            User::PasswordMetaData pd(o);
            ret.password[Mechanism::SCRAM_SHA512] = pd;
        } else if (label == "sha256") {
            User::PasswordMetaData pd(o);
            ret.password[Mechanism::SCRAM_SHA256] = pd;
        } else if (label == "sha1") {
            User::PasswordMetaData pd(o);
            ret.password[Mechanism::SCRAM_SHA1] = pd;
        } else if (label == "plain") {
            User::PasswordMetaData pd(
                    Couchbase::Base64::decode(o->valuestring));
            ret.password[Mechanism::PLAIN] = pd;
        } else {
            throw std::runtime_error(
                    "cb::cbsasl::UserFactory::create: Invalid "
                    "label \"" +
                    label + "\" specified");
        }
    }

    return ret;
}

void UserFactory::setDefaultHmacIterationCount(int count) {
    IterationCount.store(count);
}

void UserFactory::setScramshaFallbackSalt(const std::string& salt) {
    scramsha_fallback_salt.set(salt);
}

void User::generateSecrets(const Mechanism& mech, const std::string& passwd) {
    std::vector<uint8_t> salt;
    std::string encodedSalt;
    cb::crypto::Algorithm algorithm = cb::crypto::Algorithm::MD5;

    switch (mech) {
    case Mechanism::SCRAM_SHA512:
        if (dummy) {
            auto fallback = scramsha_fallback_salt.get();
            auto hs_salt =
                    cb::crypto::HMAC(cb::crypto::Algorithm::SHA512,
                                     username,
                                     {reinterpret_cast<char*>(fallback.data()),
                                      fallback.size()});
            std::copy(hs_salt.begin(), hs_salt.end(), std::back_inserter(salt));
        } else {
            salt.resize(cb::crypto::SHA512_DIGEST_SIZE);
        }
        algorithm = cb::crypto::Algorithm::SHA512;
        break;
    case Mechanism::SCRAM_SHA256:
        if (dummy) {
            auto fallback = scramsha_fallback_salt.get();
            auto hs_salt =
                    cb::crypto::HMAC(cb::crypto::Algorithm::SHA256,
                                     username,
                                     {reinterpret_cast<char*>(fallback.data()),
                                      fallback.size()});
            std::copy(hs_salt.begin(), hs_salt.end(), std::back_inserter(salt));
        } else {
            salt.resize(cb::crypto::SHA256_DIGEST_SIZE);
        }
        algorithm = cb::crypto::Algorithm::SHA256;
        break;
    case Mechanism::SCRAM_SHA1:
        if (dummy) {
            auto fallback = scramsha_fallback_salt.get();
            auto hs_salt =
                    cb::crypto::HMAC(cb::crypto::Algorithm::SHA1,
                                     username,
                                     {reinterpret_cast<char*>(fallback.data()),
                                      fallback.size()});
            std::copy(hs_salt.begin(), hs_salt.end(), std::back_inserter(salt));
        } else {
            salt.resize(cb::crypto::SHA1_DIGEST_SIZE);
        }
        algorithm = cb::crypto::Algorithm::SHA1;
        break;
    case Mechanism::PLAIN:
        throw std::logic_error(
                "cb::cbsasl::User::generateSecrets invalid algorithm");
    }

    if (algorithm == cb::crypto::Algorithm::MD5) {
        // gcc7 complains that algorithm may have been uninitialized when we
        // used it below. This would happen if the user provided a mech
        // which isn't handled above. If that happens we should just
        // throw an exception.
        throw std::invalid_argument(
                "cb::sasl::User::generateSecrets: invalid mechanism provided");
    }

    if (salt.empty()) {
        throw std::logic_error(
                "cb::cbsasl::User::generateSecrets invalid algorithm");
    }

    if (dummy) {
        using Couchbase::Base64::encode;
        encodedSalt = encode(
                std::string{reinterpret_cast<char*>(salt.data()), salt.size()});
    } else {
        generateSalt(salt, encodedSalt);
    }

    auto digest = cb::crypto::PBKDF2_HMAC(
            algorithm,
            passwd,
            {reinterpret_cast<const char*>(salt.data()), salt.size()},
            IterationCount);

    password[mech] = PasswordMetaData(digest, encodedSalt, IterationCount);
}

User::PasswordMetaData::PasswordMetaData(cJSON* obj) {
    if (obj->type != cJSON_Object) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: invalid"
                " object type");
    }

    auto* h = cJSON_GetObjectItem(obj, "h");
    auto* s = cJSON_GetObjectItem(obj, "s");
    auto* i = cJSON_GetObjectItem(obj, "i");

    if (h == nullptr || s == nullptr || i == nullptr) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: missing "
                "mandatory attributes");
    }

    if (h->type != cJSON_String) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: hash"
                " should be a string");
    }

    if (s->type != cJSON_String) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: salt"
                " should be a string");
    }

    if (i->type != cJSON_Number) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: iteration"
                " count should be a number");
    }

    if (cJSON_GetArraySize(obj) != 3) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: invalid "
                "number of labels specified");
    }

    salt.assign(s->valuestring);
    // validate that we may decode the salt
    Couchbase::Base64::decode(salt);
    password.assign(Couchbase::Base64::decode(h->valuestring));
    iteration_count = gsl::narrow<int>(i->valueint);
    if (iteration_count < 0) {
        throw std::runtime_error(
                "cb::cbsasl::User::PasswordMetaData: iteration "
                "count must be positive");
    }
}

cJSON* User::PasswordMetaData::to_json() const {
    auto* ret = cJSON_CreateObject();
    std::string s((char*)password.data(), password.size());
    cJSON_AddStringToObject(ret, "h", Couchbase::Base64::encode(s).c_str());
    cJSON_AddStringToObject(ret, "s", salt.c_str());
    cJSON_AddNumberToObject(ret, "i", iteration_count);

    return ret;
}

unique_cJSON_ptr User::to_json() const {
    auto* ret = cJSON_CreateObject();

    cJSON_AddStringToObject(ret, "n", username.c_str());
    for (auto& e : password) {
        auto* obj = e.second.to_json();
        switch (e.first) {
        case Mechanism::PLAIN:
            cJSON_AddStringToObject(
                    ret, "plain", cJSON_GetObjectItem(obj, "h")->valuestring);
            cJSON_Delete(obj);
            break;

        case Mechanism::SCRAM_SHA512:
            cJSON_AddItemToObject(ret, "sha512", obj);
            break;

        case Mechanism::SCRAM_SHA256:
            cJSON_AddItemToObject(ret, "sha256", obj);
            break;
        case Mechanism::SCRAM_SHA1:
            cJSON_AddItemToObject(ret, "sha1", obj);
            break;
        default:
            throw std::runtime_error(
                    "cb::cbsasl::User::toJSON(): Unsupported mech");
        }
    }

    return unique_cJSON_ptr(ret);
}

std::string User::to_string() const {
    return ::to_string(to_json(), false);
}

const User::PasswordMetaData& User::getPassword(const Mechanism& mech) const {
    const auto iter = password.find(mech);

    if (iter == password.end()) {
        throw std::invalid_argument(
                "cb::cbsasl::User::getPassword: requested "
                "mechanism not available");
    } else {
        return iter->second;
    }
}

} // namespace pwdb
} // namespace sasl
} // namespace cb
