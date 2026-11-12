// hull:smtp - direct SMTP delivery (host gated by manifest.hosts).
declare module "hull:smtp" {
    /** Options of `smtp.send` (snake_case, as the Lua twin). */
    export interface SmtpSendOptions {
        /** SMTP server host. */
        host: string;
        /** Port (default 587; an integer 1..65535). */
        port?: number;
        /** AUTH username. */
        username?: string;
        /** AUTH password. */
        password?: string;
        /** STARTTLS / TLS (boolean or 0 / 1). */
        tls?: boolean | number;
        /** Sender address. */
        from: string;
        /** Recipient address. */
        to: string;
        /** Subject line. */
        subject: string;
        /** Message body. */
        body: string;
        /** Body Content-Type (default "text/plain"). */
        content_type?: string;
        /** Reply-To address. */
        reply_to?: string;
        /** Carbon-copy recipients (at most 1000 strings). */
        cc?: string[] | null;
    }
    /** The outcome of a send: never rejected for a delivery failure. */
    export type SmtpResult = { ok: true } | { ok: false; error: string };
    /** The `hull:smtp` module object. */
    export interface SmtpModule {
        /** Send one message; resolves `{ ok, error? }`. Throws a TypeError for bad options and rejects with a `Hull.RefusalError` when refused. */
        send(opts: SmtpSendOptions): Promise<SmtpResult>;
    }
    /** Direct SMTP. */
    export const smtp: SmtpModule;
}
