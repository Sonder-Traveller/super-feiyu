#!/usr/bin/env bash
cd ~/super-feiyu
grep -n "mock_server\|18080\|18081\|spawn\|subprocess\|mock" tests/test_e2e.cpp | head -10
echo "--- mock server running?"
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:18080/chat/completions -X POST -d "{}" 2>&1 | head -1
echo
ss -tlnp 2>/dev/null | grep 1808