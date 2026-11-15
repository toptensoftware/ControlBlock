// Hardware test layout for the net ops (httpGet, httpPost, udpPacket, sendWol) against
// examples/test-server/test-server.mjs. The server's address comes from test-common.mjs
// (CB_TEST_SERVER, CB_TEST_SERVER_NAME). Bake and upload from the repo root:
//     node cli/cli.js upload examples/net-test.mjs

import { HTTP, SERVER, SERVER_NAME } from "./test-common.mjs";

const get = path => ({ op: "httpGet", url: `${HTTP}${path}` });
const post = (body, more) => ({ op: "httpPost", url: `${HTTP}/post`, body, ...more });
const udp = (host, data) => ({ op: "udpPacket", host, port: 9999, data });

export default {
    version: 1,
    defaults: { color: "#22262e", pressed: { color: "#3a86ff" } },
    home: {
        keys: [
            { label: "GET\nheaders", tap: { ...get("/get?x=1"), headers: { "X-Test": "one", "X-Other": "two" } } },
            { label: "POST\nform", tap: post("a=1&b=2", { headers: { "X-Test": "post" } }) },
            { label: "POST\njson", tap: post("{\"k\":1}", { contentType: "application/json" }) },
            { label: "GET\nredirect", tap: get("/redirect") },
            { label: "GET\n404", tap: get("/nope") },

            { label: "GET\nhang", tap: get("/hang") },
            // an unroutable address: the request times out
            { label: "GET\ndropped", tap: { op: "httpGet", url: "http://10.255.255.1/" } },
            { label: "GET\n5 MB", tap: get("/big") },
            // the same server by name: a host change reopens the connection
            { label: "GET\n2nd host", tap: { op: "httpGet", url: `http://${SERVER_NAME}:8000/get?host=2` } },
            { label: "GET\nslow", tap: get("/slow") },

            { label: "UDP\nIP", tap: udp(SERVER, "hello by ip") },
            { label: "UDP\nname", tap: udp(SERVER_NAME, "hello by name") },
            { label: "UDP\nhex", tap: udp(SERVER, { hex: "DEADBEEF00FF" }) },
            { label: "WoL\nsubnet", tap: { op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" } },
            // 12 slow requests at once: the net queue (8) drops the oldest 4
            { label: "Flood\n12 slow", tap: Array.from({ length: 12 }, (_, i) => get(`/slow?n=${i + 1}`)) },
        ],
    },
};
