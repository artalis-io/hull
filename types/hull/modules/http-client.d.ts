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
