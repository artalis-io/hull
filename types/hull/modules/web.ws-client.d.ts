// hull:web:ws-client - outbound WebSocket connections (host must be in manifest `hosts`).
declare module "hull:web:ws-client" {
    /** WebSocket client. */
    export const wsClient: {
        /**
         * Connect to a `ws://` / `wss://` URL; callbacks fire on the event loop. Throws for a
         * host outside `manifest.hosts`, and a `busy` refusal past 64 live clients.
         */
        connect(url: string, handlers: Hull.WsClientHandlers): Hull.WsClientConnection;
    };
}
