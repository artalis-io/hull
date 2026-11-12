// hull:email - outbound email through SMTP or an HTTP provider (Postmark, SendGrid, Resend).
declare module "hull:email" {
    /** The delivery backend. */
    export type EmailProvider = "smtp" | "postmark" | "sendgrid" | "resend";
    /** Options of `email.send` (snake_case, as the Lua twin). */
    export interface EmailSendOptions {
        /** Delivery backend (default "smtp"). */
        provider?: EmailProvider;
        /** Sender address. */
        from: string;
        /** Recipient address (one). */
        to: string;
        /** Subject line. */
        subject: string;
        /** Message body. */
        body: string;
        /** Carbon-copy recipients: one address or a list. */
        cc?: string | string[] | null;
        /** Reply-To address. */
        reply_to?: string | null;
        /** Body type (default "text/plain"). */
        content_type?: "text/plain" | "text/html";
        /** API key; required by the HTTP providers. */
        api_key?: string;
        /** SMTP server host (provider "smtp"). */
        smtp_host?: string;
        /** SMTP port (default 587). */
        smtp_port?: number;
        /** SMTP username. */
        smtp_user?: string;
        /** SMTP password. */
        smtp_pass?: string;
        /** Use TLS for SMTP (default true). */
        smtp_tls?: boolean;
    }
    /** The coded Error `email.send` rejects with (a runtime refusal rejects as `Hull.RefusalError`). */
    export interface EmailError extends Error {
        /** Stable failure code. */
        code: "invalid_argument" | "unknown_provider" | "delivery_failed";
    }
    /** The `hull:email` module object. */
    export interface EmailModule {
        /** Send one message; resolves `true`, rejects with an `EmailError` (or a `Hull.RefusalError`). */
        send(opts: EmailSendOptions): Promise<true>;
    }
    /** Outbound email. */
    export const email: EmailModule;
}
