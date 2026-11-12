// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// commit_backoff.hpp: the attempts a commit gets (20 locally, 200 on an object store, NANOLANCE_COMMIT_ATTEMPTS
// within 1..100,000 overriding both), the pause before each retry (bounded by the ceiling), and that forked
// processes draw different ids from new_uuid, which names transactions and branches (seeded per call, not copied
// into the child).

#include "../posix_env.hpp"
#include "nanolance/commit_backoff.hpp"
#include "base/uuid.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

long pause_ms(int attempt, bool object_store) {
    const auto start = std::chrono::steady_clock::now();
    nano_lance::commit_back_off(attempt, object_store);
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

}  // namespace

std::string uuid_bytes() {
    const auto id = nano_lance::new_uuid();
    return std::string(reinterpret_cast<const char*>(id.data()), id.size());
}

int main() {
    using nano_lance::commit_attempts;
    unsetenv("NANOLANCE_COMMIT_ATTEMPTS");
    require(commit_attempts(false) == 20, "20 attempts locally (Lance's conflict_retries)");
    require(commit_attempts(true) == 200, "200 attempts on an object store");
    setenv("NANOLANCE_COMMIT_ATTEMPTS", "7", 1);
    require(commit_attempts(false) == 7 && commit_attempts(true) == 7, "the environment overrides both");
    for (const char* bad : {"0", "100001", "-3", "abc", "12x", ""}) {
        setenv("NANOLANCE_COMMIT_ATTEMPTS", bad, 1);
        require(commit_attempts(true) == 200, std::string("an invalid override is ignored: '") + bad + "'");
    }
    unsetenv("NANOLANCE_COMMIT_ATTEMPTS");

    // Pauses stay within their ceilings (plus scheduling slack): 1 ms then up to 50 ms locally; 25 ms doubling to
    // 2 s on an object store.
    require(pause_ms(0, false) < 1 + 200, "local first pause");
    require(pause_ms(0, true) < 25 + 200, "object-store first pause");
    for (int k = 0; k < 3; ++k) {
        require(pause_ms(40, false) < 50 + 200, "the local ceiling is 50 ms");
    }

#ifdef _WIN32
    // The fork case below needs fork(), which Windows does not have. The checks above ran.
    std::cout << "skipped on Windows: needs fork (transaction ids drawn in forked children)\n";
    std::cout << "commit backoff: attempts, pauses OK\n";
    return 0;
#else
    // Transaction ids drawn in forked children after the parent drew one: all different.
    const std::string parent = uuid_bytes();
    int fds[2];
    require(pipe(fds) == 0, "pipe");
    constexpr int kChildren = 8;
    for (int k = 0; k < kChildren; ++k) {
        if (fork() == 0) {
            const std::string id = uuid_bytes();
            (void)!write(fds[1], id.data(), id.size());
            std::_Exit(0);
        }
    }
    for (int k = 0; k < kChildren; ++k) {
        int status = 0;
        wait(&status);
    }
    close(fds[1]);
    std::string all(kChildren * parent.size(), '\0');
    std::size_t got = 0;
    for (ssize_t n = 0; got < all.size() && (n = read(fds[0], all.data() + got, all.size() - got)) > 0;) {
        got += static_cast<std::size_t>(n);
    }
    close(fds[0]);
    require(got == all.size(), "every child reported its id");
    for (int a = 0; a < kChildren; ++a) {
        const auto ida = all.substr(static_cast<std::size_t>(a) * parent.size(), parent.size());
        require(ida != parent, "a child's id differs from the parent's");
        for (int b = a + 1; b < kChildren; ++b) {
            require(ida != all.substr(static_cast<std::size_t>(b) * parent.size(), parent.size()),
                    "two forked children drew the same transaction id");
        }
    }
    std::cout << "commit backoff: attempts, pauses, fork-distinct transaction ids OK\n";
    return 0;
#endif
}
