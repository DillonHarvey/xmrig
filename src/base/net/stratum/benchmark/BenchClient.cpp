/* XMRig
 * Copyright (c) 2018-2020 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2020 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "base/net/stratum/benchmark/BenchClient.h"
#include "3rdparty/fmt/core.h"
#include "3rdparty/rapidjson/document.h"
#include "backend/common/benchmark/BenchState.h"
#include "backend/common/interfaces/IBackend.h"
#include "backend/cpu/Cpu.h"
#include "base/io/json/Json.h"
#include "base/io/log/Log.h"
#include "base/io/log/Tags.h"
#include "base/kernel/interfaces/IClientListener.h"
#include "base/net/http/Fetch.h"
#include "base/net/http/HttpData.h"
#include "base/net/http/HttpListener.h"
#include "base/net/stratum/benchmark/BenchConfig.h"
#include "version.h"


xmrig::BenchClient::BenchClient(const std::shared_ptr<BenchConfig> &benchmark, IClientListener* listener) :
    m_listener(listener),
    m_benchmark(benchmark),
    m_hash(benchmark->hash())
{
    std::vector<char> blob(112 * 2 + 1, '0');
    blob.back() = '\0';

    m_job.setBlob(blob.data());
    m_job.setAlgorithm(m_benchmark->algorithm());
    m_job.setDiff(std::numeric_limits<uint64_t>::max());
    m_job.setHeight(1);
    m_job.setBenchSize(m_benchmark->size());

    BenchState::setListener(this);

#   ifdef XMRIG_FEATURE_HTTP
    if (m_benchmark->isSubmit()) {
        m_mode = ONLINE_BENCH;

        return;
    }

    if (!m_benchmark->id().isEmpty()) {
        m_job.setId(m_benchmark->id());
        m_token = m_benchmark->token();
        m_mode  = ONLINE_VERIFY;

        return;
    }
#   endif

    m_job.setId("00000000");

    if (m_hash && m_job.setSeedHash(m_benchmark->seed())) {
        m_mode = STATIC_VERIFY;

        return;
    }

    blob[Job::kMaxSeedSize * 2] = '\0';
    m_job.setSeedHash(blob.data());
}


xmrig::BenchClient::~BenchClient()
{
    BenchState::destroy();
}


void xmrig::BenchClient::connect()
{
#   ifdef XMRIG_FEATURE_HTTP
    switch (m_mode) {
    case STATIC_BENCH:
    case STATIC_VERIFY:
        return start();

    case ONLINE_BENCH:
        return createBench();

    case ONLINE_VERIFY:
        return getBench();
    }
#   else
    start();
#   endif
}


void xmrig::BenchClient::setPool(const Pool &pool)
{
    m_pool = pool;
}


void xmrig::BenchClient::onBenchDone(uint64_t result, uint64_t ts)
{
#   ifdef XMRIG_FEATURE_HTTP
    if (!m_token.isEmpty()) {
        m_doneTime = ts;

        rapidjson::Document doc(rapidjson::kObjectType);
        auto &allocator = doc.GetAllocator();

        doc.AddMember("steady_done_ts", m_doneTime, allocator);
        doc.AddMember("hash",           rapidjson::Value(fmt::format("{:016X}", result).c_str(), allocator), allocator);
        doc.AddMember("backend",        m_backend->toJSON(doc), allocator);

        update(doc);
    }
#   endif

    const uint64_t ref = referenceHash();
    const char *color  = ref ? ((result == ref) ? GREEN_BOLD_S : RED_BOLD_S) : BLACK_BOLD_S;

    LOG_NOTICE("%s " WHITE_BOLD("benchmark finished in ") CYAN_BOLD("%.3f seconds") WHITE_BOLD_S " hash sum = " CLEAR "%s%016" PRIX64 CLEAR, Tags::bench(), static_cast<double>(ts - m_startTime) / 1000.0, color, result);

    if (m_mode != ONLINE_BENCH) {
        printExit();
    }
}


void xmrig::BenchClient::onBenchStart(uint64_t ts, uint32_t threads, const IBackend *backend)
{
    m_startTime = ts;
    m_threads   = threads;
    m_backend   = backend;

#   ifdef XMRIG_FEATURE_HTTP
    if (m_mode == ONLINE_BENCH) {
        rapidjson::Document doc(rapidjson::kObjectType);
        auto &allocator = doc.GetAllocator();

        doc.AddMember("threads",            threads, allocator);
        doc.AddMember("steady_start_ts",    m_startTime, allocator);

        update(doc);
    }
#   endif
}


void xmrig::BenchClient::onHttpData(const HttpData &data)
{
#   ifdef XMRIG_FEATURE_HTTP
    rapidjson::Document doc;

    try {
        doc = data.json();
    } catch (const std::exception &ex) {
        return setError(ex.what());
    }

    if (data.status != 200) {
        return setError(data.statusName());
    }

    if (m_doneTime) {
        LOG_NOTICE("%s " WHITE_BOLD("benchmark submitted ") CYAN_BOLD("https://xmrig.com/benchmark/%s"), Tags::bench(), m_job.id().data());
        printExit();

        return;
    }

    if (m_startTime) {
        return;
    }

    if (m_mode == ONLINE_BENCH) {
        startBench(doc);
    }
    else {
        startVerify(doc);
    }
#   endif
}


uint64_t xmrig::BenchClient::referenceHash() const
{
    if (m_hash || m_mode == ONLINE_BENCH) {
        return m_hash;
    }

    return BenchState::referenceHash(m_job.algorithm(), m_job.benchSize(), m_threads);
}


void xmrig::BenchClient::printExit()
{
    LOG_INFO("%s " WHITE_BOLD("press ") MAGENTA_BOLD("Ctrl+C") WHITE_BOLD(" to exit"), Tags::bench());
}


void xmrig::BenchClient::start()
{
    m_listener->onLoginSuccess(this);
    m_listener->onJobReceived(this, m_job, rapidjson::Value());
}



#ifdef XMRIG_FEATURE_HTTP
void xmrig::BenchClient::createBench()
{
    createHttpListener();

    using namespace rapidjson;

    Document doc(kObjectType);
    auto &allocator = doc.GetAllocator();

    doc.AddMember(StringRef(BenchConfig::kSize), m_benchmark->size(), allocator);
    doc.AddMember(StringRef(BenchConfig::kAlgo), m_benchmark->algorithm().toJSON(), allocator);
    doc.AddMember("version",                     APP_VERSION, allocator);
    doc.AddMember("cpu",                         Cpu::toJSON(doc), allocator);

    FetchRequest req(HTTP_POST, BenchConfig::kApiHost, BenchConfig::kApiPort, "/1/benchmark", doc, BenchConfig::kApiTLS, true);
    fetch(std::move(req), m_httpListener);
}


void xmrig::BenchClient::createHttpListener()
{
    if (!m_httpListener) {
        m_httpListener = std::make_shared<HttpListener>(this, Tags::bench());
    }
}


void xmrig::BenchClient::getBench()
{
    createHttpListener();

    FetchRequest req(HTTP_GET, BenchConfig::kApiHost, BenchConfig::kApiPort, fmt::format("/1/benchmark/{}", m_job.id()).c_str(), BenchConfig::kApiTLS, true);
    fetch(std::move(req), m_httpListener);
}


void xmrig::BenchClient::setError(const char *message)
{
    LOG_ERR("%s " RED("benchmark failed ") RED_BOLD("\"%s\""), Tags::bench(), message);
}


void xmrig::BenchClient::startBench(const rapidjson::Value &value)
{
    m_job.setId(Json::getString(value, BenchConfig::kId));
    m_job.setSeedHash(Json::getString(value, BenchConfig::kSeed));

    m_token = Json::getString(value, BenchConfig::kToken);

    start();
}


void xmrig::BenchClient::startVerify(const rapidjson::Value &value)
{
    const char *hash = Json::getString(value, BenchConfig::kHash);
    if (hash) {
        m_hash = strtoull(hash, nullptr, 16);
    }

    m_job.setAlgorithm(Json::getString(value, BenchConfig::kAlgo));
    m_job.setSeedHash(Json::getString(value, BenchConfig::kSeed));
    m_job.setBenchSize(Json::getUint(value, BenchConfig::kSize));

    start();
}


void xmrig::BenchClient::update(const rapidjson::Value &body)
{
    assert(!m_token.isEmpty());

    FetchRequest req(HTTP_PATCH, BenchConfig::kApiHost, BenchConfig::kApiPort, fmt::format("/1/benchmark/{}", m_job.id()).c_str(), body, BenchConfig::kApiTLS, true);
    req.headers.insert({ "Authorization", fmt::format("Bearer {}", m_token)});

    fetch(std::move(req), m_httpListener);
}
#endif