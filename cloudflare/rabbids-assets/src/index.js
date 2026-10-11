// Serve the extracted game folder directly from a private Cloudflare R2 bucket.
// GET/HEAD support HTTP byte ranges for the WebAssembly game runtime.
const ALLOWED_ORIGINS = new Set([
  'https://jmhq20120212-cmd.github.io',
  'http://localhost:8991',
  'http://127.0.0.1:8991',
]);

function corsHeaders(request) {
  const headers = new Headers({
    'Vary': 'Origin',
    'Cross-Origin-Resource-Policy': 'cross-origin',
    'Access-Control-Expose-Headers': 'Content-Length, Content-Range, Accept-Ranges, ETag',
    'Accept-Ranges': 'bytes',
  });
  const origin = request.headers.get('Origin');
  if (ALLOWED_ORIGINS.has(origin)) headers.set('Access-Control-Allow-Origin', origin);
  return headers;
}

function safeKey(url) {
  let key;
  try { key = decodeURIComponent(url.pathname).replace(/^\/+/, ''); }
  catch { return null; }
  if (!key.startsWith('game/') || key.includes('\\') ||
      key.split('/').some((segment) => !segment || segment === '.' || segment === '..' || /[\x00-\x1F]/.test(segment))) return null;
  return key;
}

export default {
  async fetch(request, env) {
    const headers = corsHeaders(request);
    const method = request.method;
    if (method === 'OPTIONS') {
      const origin = request.headers.get('Origin');
      if (!ALLOWED_ORIGINS.has(origin)) return new Response(null, { status: 403, headers });
      headers.set('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS');
      headers.set('Access-Control-Allow-Headers', 'Range, If-Range');
      headers.set('Access-Control-Max-Age', '7200');
      return new Response(null, { status: 204, headers });
    }
    if (method !== 'GET' && method !== 'HEAD') return new Response('Method not allowed', { status: 405, headers });
    const key = safeKey(new URL(request.url));
    if (!key) return new Response('Not found', { status: 404, headers });

    try {
      const result = method === 'HEAD'
        ? await env.GAME_DATA.head(key)
        : await env.GAME_DATA.get(key, { range: request.headers });
      if (!result) return new Response('Not found', { status: 404, headers });
      result.writeHttpMetadata(headers);
      if (!headers.has('Content-Type')) headers.set('Content-Type', 'application/octet-stream');
      headers.set('ETag', result.httpEtag);
      headers.set('Cache-Control', 'public, max-age=3600');
      const requestedRange = request.headers.has('Range');
      const actualRange = result.range;
      if (method === 'GET' && requestedRange && actualRange && typeof actualRange.offset === 'number' &&
          typeof actualRange.length === 'number') {
        headers.set('Content-Range', `bytes ${actualRange.offset}-${actualRange.offset + actualRange.length - 1}/${result.size}`);
        headers.set('Content-Length', String(actualRange.length));
        return new Response(result.body, { status: 206, headers });
      }
      headers.set('Content-Length', String(result.size));
      return new Response(method === 'HEAD' ? null : result.body, { status: 200, headers });
    } catch (error) {
      // R2 throws for invalid Range requests; expose standard HTTP 416 response.
      if (request.headers.has('Range')) {
        const obj = await env.GAME_DATA.head(key);
        if (obj) {
          headers.set('Content-Range', `bytes */${obj.size}`);
          return new Response(null, { status: 416, headers });
        }
      }
      return new Response('Storage temporarily unavailable', { status: 503, headers });
    }
  },
};
