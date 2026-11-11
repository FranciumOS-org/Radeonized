// Radeonize report intake: a Cloudflare Worker that stores diagnostics reports in R2.
//
//   POST /report        public. Accepts one text/plain report up to 2 MB, stores it, returns its ID.
//   GET  /admin         PRIVATE. Lists recent reports.
//   GET  /admin/view/K  PRIVATE. Shows one report (K = the key from the list).
//
// /admin must sit behind Cloudflare Access (allow only your own email), configured in the
// Zero Trust dashboard as an Access application for <your-host>/admin*. Only /report is
// meant to be public. Rate limiting for /report is set as a Cloudflare rate-limiting rule.

const MAX_BYTES = 2 * 1024 * 1024;

function esc(s) {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "POST" && url.pathname === "/report") {
      const type = request.headers.get("content-type") || "";
      if (!type.startsWith("text/plain")) return new Response("expected text/plain", { status: 415 });

      const declared = Number(request.headers.get("content-length") || 0);
      if (declared > MAX_BYTES) return new Response("too large", { status: 413 });

      const body = await request.text();
      if (new TextEncoder().encode(body).length > MAX_BYTES) return new Response("too large", { status: 413 });
      if (body.trim().length === 0) return new Response("empty", { status: 400 });

      const id = crypto.randomUUID();
      const day = new Date().toISOString().slice(0, 10);
      await env.REPORTS.put(`reports/${day}/${id}.txt`, body, {
        httpMetadata: { contentType: "text/plain; charset=utf-8" },
        customMetadata: { received: new Date().toISOString() },
      });
      return new Response(id, { status: 201, headers: { "content-type": "text/plain" } });
    }

    if (request.method === "GET" && url.pathname === "/admin") {
      const listed = await env.REPORTS.list({ prefix: "reports/", limit: 200 });
      const rows = listed.objects
        .sort((a, b) => (a.key < b.key ? 1 : -1))
        .map((o) => `<li><a href="/admin/view/${encodeURIComponent(o.key)}">${esc(o.key)}</a> (${o.size} bytes)</li>`)
        .join("");
      return page("Radeonize reports", `<ul>${rows || "<li>No reports yet.</li>"}</ul>`);
    }

    if (request.method === "GET" && url.pathname.startsWith("/admin/view/")) {
      const key = decodeURIComponent(url.pathname.slice("/admin/view/".length));
      if (!/^reports\/[0-9-]{10}\/[0-9a-f-]{36}\.txt$/.test(key)) return new Response("bad key", { status: 400 });
      const obj = await env.REPORTS.get(key);
      if (!obj) return new Response("not found", { status: 404 });
      return page(esc(key), `<pre>${esc(await obj.text())}</pre>`);
    }

    return new Response("not found", { status: 404 });
  },
};

function page(title, inner) {
  return new Response(
    `<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">` +
      `<title>${title}</title><style>body{font:14px system-ui;margin:16px}pre{white-space:pre-wrap}</style>` +
      `<h2>${title}</h2>${inner}`,
    { headers: { "content-type": "text/html; charset=utf-8", "x-robots-tag": "noindex" } }
  );
}
