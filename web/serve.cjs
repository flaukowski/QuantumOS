const http = require("http"), fs = require("fs"), path = require("path");
const types = {
  ".html":"text/html", ".js":"text/javascript", ".mjs":"text/javascript",
  ".wasm":"application/wasm", ".data":"application/octet-stream",
  ".css":"text/css", ".elf32":"application/octet-stream",
};
const PORT = process.argv[2] ? parseInt(process.argv[2]) : 8901;
http.createServer((req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
  let p = decodeURIComponent(req.url.split("?")[0]);
  if (p === "/") p = "/spike.html";
  const fp = path.join(__dirname, p);
  if (!fp.startsWith(__dirname)) { res.statusCode = 403; return res.end("no"); }
  fs.readFile(fp, (e, b) => {
    if (e) { res.statusCode = 404; return res.end("404 " + p); }
    res.setHeader("Content-Type", types[path.extname(fp)] || "application/octet-stream");
    res.end(b);
  });
}).listen(PORT, () => console.log("serving " + __dirname + " on " + PORT));
