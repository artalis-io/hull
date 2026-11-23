// hull:http-client - outbound HTTP/HTTPS, gated by manifest.hosts.
declare module "hull:http-client" {
    /** Request headers: name -> value (values are converted with String()). Hull-owned headers (Host, Content-Length, Transfer-Encoding, Connection, ...) throw. */
    export type HttpHeaders = { [name: string]: string | number | boolean };
    /** A request body: a string (sent as UTF-8), an ArrayBuffer or a typed array; null / undefined sends none. */
    export type HttpBody = string | ArrayBuffer | ArrayBufferView | null | undefined;
    /** Options of the calls that take their body as an argument. */
    export interface HttpOptions {
        /** Extra request headers. */
        headers?: HttpHeaders;
        /** One deadline (ms, >= 1) for the whole request incl. redirects; default manifest `http.timeoutMs`, else 30 s. Clamped to 60 s (sync) / 10 min (async). */
        timeoutMs?: number | null;
    }
    /** Options of `request` / `async.request` / `async.get` / `async.delete`, which may carry the body. */
    export interface HttpRequestOptions extends HttpOptions {
        /** The request body. */
        body?: HttpBody;
    }
    /** A received HTTP response (any status; only a transport failure throws). */
    export interface HttpResponse {
        /** The HTTP status code. */
        status: number;
        /** The body as a string ("" when empty). */
        body: string;
        /** Response headers by lowercased name. */
        headers: { [name: string]: string };
    }
    /** Options of `async.stream`. */
    export interface HttpStreamOptions extends HttpRequestOptions {
        /** Deadline (ms, >= 1) up to the response headers (default manifest `http.timeoutMs`, else 30 s; clamped to 10 min). */
        timeoutMs?: number | null;
        /** Longest gap (ms, >= 1) between two body arrivals after the headers; default 60 000, at most 1 h. */
        idleTimeoutMs?: number | null;
        /** The whole stream's life (ms, >= 1) from its start, headers included; past it the stream ends with code "life". Default 600 000 (10 min), clamped to 24 h. */
        lifeMs?: number | null;
        /** Ends the stream with code "too_large" once the body passes this many bytes; default no limit. */
        maxBytes?: number | null;
        /** Unread bytes held for a slow reader before the stream ends with code "overflow"; default 1 MiB, 16 KiB .. 16 MiB. */
        bufferBytes?: number | null;
    }
    /** A streamed response: status and headers now, the body as it arrives. A read that ends the stream with an error throws an Error whose `code` is "timeout", "idle", "life", "overflow", "too_large", "closed" or "io_error". */
    export interface HttpStream {
        /** The HTTP status code. */
        readonly status: number;
        /** Response headers by lowercased name. */
        readonly headers: { [name: string]: string };
        /** The next body bytes; null at the end of the body. */
        read(): Promise<ArrayBuffer | null>;
        /** The next body text (UTF-8; a character split across chunks waits for the rest); null at the end. */
        readText(): Promise<string | null>;
        /** Stop the request; a waiting read finds the stream closed. */
        close(): void;
        /** Async-iterator step over the byte chunks. */
        next(): Promise<IteratorResult<ArrayBuffer, undefined>>;
        /** Leaving a for-await early closes the stream. */
        return(): Promise<IteratorResult<ArrayBuffer, undefined>>;
        /** `for await (const chunk of res)`. */
        [Symbol.asyncIterator](): HttpStream;
    }
    /** Non-blocking variants: each parks the caller and resolves to the response. */
    export interface HttpClientAsync {
        /** Send `method` to `url`; resolves to the response. Refused (`busy`) past 64 fetches in flight. */
        request(method: string, url: string, opts?: HttpRequestOptions): Promise<HttpResponse>;
        /** GET `url`. */
        get(url: string, opts?: HttpRequestOptions): Promise<HttpResponse>;
        /** POST `body` to `url`. */
        post(url: string, body?: HttpBody, opts?: HttpOptions): Promise<HttpResponse>;
        /** PUT `body` to `url`. */
        put(url: string, body?: HttpBody, opts?: HttpOptions): Promise<HttpResponse>;
        /** PATCH `body` to `url`. */
        patch(url: string, body?: HttpBody, opts?: HttpOptions): Promise<HttpResponse>;
        /** DELETE `url`. */
        delete(url: string, opts?: HttpRequestOptions): Promise<HttpResponse>;
        /** Send `method` to `url`; resolves to the response as soon as its headers arrive, the body read from it. No redirects. Counts as one of the 64 requests in flight while open. */
        stream(method: string, url: string, opts?: HttpStreamOptions): Promise<HttpStream>;
    }
    /** The `hull:http-client` module object. The sync calls block the event loop; prefer `async`. */
    export interface HttpClient {
        /** Send `method` to `url` synchronously; throws on a transport failure. */
        request(method: string, url: string, opts?: HttpRequestOptions): HttpResponse;
        /** GET `url` synchronously. */
        get(url: string, opts?: HttpOptions): HttpResponse;
        /** POST `body` to `url` synchronously. */
        post(url: string, body?: HttpBody, opts?: HttpOptions): HttpResponse;
        /** PUT `body` to `url` synchronously. */
        put(url: string, body?: HttpBody, opts?: HttpOptions): HttpResponse;
        /** PATCH `body` to `url` synchronously. */
        patch(url: string, body?: HttpBody, opts?: HttpOptions): HttpResponse;
        /** DELETE `url` synchronously. */
        delete(url: string, opts?: HttpOptions): HttpResponse;
        /** Non-blocking variants returning Promises. */
        readonly async: HttpClientAsync;
    }
    /** The outbound HTTP client. */
    export const httpClient: HttpClient;
}
