/**
 * @file smtp_tls_test_certs.h
 * @brief FIXED test-only TLS material for the in-process mbedTLS SMTP peer.
 *
 * TEST-ONLY. Minted offline once (RSA-2048, ~30y validity) and embedded as PEM
 * literals so the TLS suite has NO external openssl process and NO network
 * dependency. NEVER compiled into a shipped binary - included only by
 * tests/hull/cap/test_smtp_transport.c.
 *
 *   HL_TLS_CA1_PEM  - CA that signed the server cert (client trusts this = success)
 *   HL_TLS_SRV_CRT  - server leaf, CN=localhost, SAN DNS:localhost + IP:127.0.0.1
 *                     (signed by CA1), so both "localhost" and "127.0.0.1" verify
 *   HL_TLS_SRV_KEY  - server private key
 *   HL_TLS_CA2_PEM  - an UNRELATED CA (client trusts this = unknown-CA failure)
 *
 * Lengths are taken with sizeof() so the trailing NUL is included, as mbedTLS PEM
 * parsing requires.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SMTP_TLS_TEST_CERTS_H
#define HL_SMTP_TLS_TEST_CERTS_H

static const char HL_TLS_CA1_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDHzCCAgegAwIBAgIUDPjDIF1HHmzXAY01Yrvbqi/RCsEwDQYJKoZIhvcNAQEL\n"
    "BQAwHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3QgQ0EgMTAgFw0yNjA5MDIxNzEw\n"
    "MzRaGA8yMDU2MDgyNTE3MTAzNFowHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3Qg\n"
    "Q0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBANZBFEhuGUbMm0OO\n"
    "GM1yhdSjYeEHCOMucnFvniC9unPOCyUCpPVMFGq/sMwWLJwK0S1KLdutxuAfSo25\n"
    "d1AZdXMI9Wt+pcGn8h0VOiXqtfWFysQA//p719+7/TKNgeFiTXvm9CVUmjg5JtkZ\n"
    "pMinJYsR89rgMyZ/eHvYXuQXb/4aBwLMTiIFHWqzgnMVQLZXLvKjdytjidkZmJrm\n"
    "aH2mWlv/c5WB8k7IrpGTnQGf0B98OvGtfUqSApsXKsBncNfVcBW8mvKirjmKnTxq\n"
    "aMzay/muqlCUcrIlbQyi3Mg7QL5kSU9EuV2qKANM4H8YWUpc67+ZMN4S0JSzsByG\n"
    "2ttC/0ECAwEAAaNTMFEwHQYDVR0OBBYEFDVDv5ut+og4U3DahLmL1BEAYHykMB8G\n"
    "A1UdIwQYMBaAFDVDv5ut+og4U3DahLmL1BEAYHykMA8GA1UdEwEB/wQFMAMBAf8w\n"
    "DQYJKoZIhvcNAQELBQADggEBAI0cgz/WJgZ5yVrL1S1u/7+czUlrWr9TP6qnD//h\n"
    "QhntQRQpGurknSKPOfiiz6cBP+Sv1uQ3g4axVBfpmeA/gYhxXiCJSKr/oej3+9VN\n"
    "MB4w6Gdqgmg82DxtXgUwS/2UoD/yjzx4aR7g7GVkJS6FEDTj88p4MSQ0/0PPTsnC\n"
    "apCnW12vaQSCoxFc8jMBhcG+K7uGXl+SjVFwP09BPjSNm1Box97Vmq0lE7+zQKAd\n"
    "h+p0XyQhzVaqGiSxay4APeoztzDFyOHKV1FRqHXpjljCYEwzF6pOrdXJS2piGv6w\n"
    "LQPFPjozhjfcx4Q4diIa5QvJvCFCYN/3mAHEEvoLebNrGWs=\n"
    "-----END CERTIFICATE-----\n"
;

static const char HL_TLS_SRV_CRT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDKzCCAhOgAwIBAgIUUlzXoJNui1PStz+yEJK5RI2QYKAwDQYJKoZIhvcNAQEL\n"
    "BQAwHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3QgQ0EgMTAgFw0yNjA5MDIxNzEw\n"
    "MzRaGA8yMDU2MDgyNTE3MTAzNFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjAN\n"
    "BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA6gYgi84Rxs52aSIZi1lcMh0CTerR\n"
    "7Ob/CTtbtld8DNrSOXmfb82GVHxY57AX62rL/eggddKcgpbzX+8pbESe7K09J6Wj\n"
    "EESjBpR6YQVfEL8LQ90aZVg+cv05b5sei8CKIMtyygJbwo6mGPBWJ4qxq26kLL5X\n"
    "xLHKUAX1E9JGUb9/Dew0Z/aSfmJZbg0PnutN6gyUUHJES7QuN4V/iw/3Xg4dTK2+\n"
    "7Gf4RdFJ/gTGpIh80j34/uf6VkNkyTpo6AA9DGjGECfcD4pAU5cBCq14Bh9jWzuO\n"
    "S39BNHkuiEjJqr53woOy7r3c4c0SPXDzdj8V0Qa/KRHAwnt1Ocxb7Zl3NQIDAQAB\n"
    "o2kwZzAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwCQYDVR0TBAIwADAdBgNV\n"
    "HQ4EFgQUP+sPsJhqd+pybV5hN1/YWbHaIigwHwYDVR0jBBgwFoAUNUO/m636iDhT\n"
    "cNqEuYvUEQBgfKQwDQYJKoZIhvcNAQELBQADggEBAGJuMCZUkvhGG1lNwPqFG0+h\n"
    "f/7xT7pm86ySe4yZ6P3ZIM80/N1jvhMWqgRPg6WGjxY5znqNaGJT+GlnzY5z1k4C\n"
    "YY1kJDIqy1+6ET+9GmZYtQHVI5xkZigPsmkYwCMaWBq/D8MALeVZe6YbPh4GqYG+\n"
    "Xn5qDsSZhiuP7aN6EQSPCh6zm+2ZpXZbPNErUR7Awslz1Kks27wXIpwrdx/SWgtz\n"
    "TFz/sai3xcuoSBMwE+3n5O46rIW8bvE3K0Pv4kmbfI9WtVjdlhi9jtYU/GnWhGd3\n"
    "K7VMLZ7WTnjzK9G1RZDNEIh8+BcWKr+ZZORxKp/qiGf5Yxb9qQyW43h38CKBnBc=\n"
    "-----END CERTIFICATE-----\n"
;

static const char HL_TLS_SRV_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDqBiCLzhHGznZp\n"
    "IhmLWVwyHQJN6tHs5v8JO1u2V3wM2tI5eZ9vzYZUfFjnsBfrasv96CB10pyClvNf\n"
    "7ylsRJ7srT0npaMQRKMGlHphBV8QvwtD3RplWD5y/Tlvmx6LwIogy3LKAlvCjqYY\n"
    "8FYnirGrbqQsvlfEscpQBfUT0kZRv38N7DRn9pJ+YlluDQ+e603qDJRQckRLtC43\n"
    "hX+LD/deDh1Mrb7sZ/hF0Un+BMakiHzSPfj+5/pWQ2TJOmjoAD0MaMYQJ9wPikBT\n"
    "lwEKrXgGH2NbO45Lf0E0eS6ISMmqvnfCg7LuvdzhzRI9cPN2PxXRBr8pEcDCe3U5\n"
    "zFvtmXc1AgMBAAECggEAIkL24VjwR9GiytYL9NeGlKykEXm7NaqE9JpM3Ud2GSCx\n"
    "Lep1MeZeUACreHISsmvehSQGmpFUyhak92rZfukV9lMPlL4efyt5TaWPvCQP7LD3\n"
    "in40ltlCPU3+6mzEnlO4NUBr5BDP085UGWsYRe47gDxwyz8rtNeNpcUVggsjMeTm\n"
    "Xrjmx5blBnkawGasHfDFC09uXO+QnxvEmuIgIHFBH8R3nJ/JOJ8N9lOV543FvlFt\n"
    "CsKwKG00MiR0DsljZXgTVRgb7qaQWGNGewlEBjkF3BitJn8VW6YZ1sv52WUwilpo\n"
    "o1J5Cm5VX49UaWhWEj5BYl1tPAMbC7/loCtYcMk6wQKBgQD14zNRGVKDl7tztlw2\n"
    "QGJ5q/ePMakmTOqbemVwUO1fW6a+USaey7sGKTetg24mCzDQwLz+mLN8C2ITA5ZL\n"
    "Q5rPkBEqFahvubUzYTsSTWqZx6gMzVNrjhJWLH/nelsv5z2ihd39No29f02ayU4c\n"
    "DfwtY/QcZJh+u2bIsxgsphIFQQKBgQDzpgW8j0Bk27IhfXYENa6M0/b4517dPDee\n"
    "+z8TgD5JgzY4f8GWmEsriHAP9wgfGV4zDqZCvFlj5vRxzZtcBSSoVVerKqMz7rFO\n"
    "0H54SBABOgQfMVmU5Wv4onhGxWF47mbCSXwVhWdPs2BxmPMaQUqVAP+tFoyqv7Cd\n"
    "qbeTso5w9QKBgHAJ9IoRhb2cV7ej8mRt9fEG4KiIslBXX9c0cCA7X83BjzrM81IL\n"
    "9s4Z3drcNkZzduzHxdYkcjQlY6zSR5tH1LSbKpcIg1VVQzGELkxqphYoGXSr4kTx\n"
    "2X0WjblF0WMEdNsnMD1+rBsadJwA+eximvN7xfFiDPJCJdVxdaRyj2eBAoGBAO26\n"
    "2zQNLg560kadDK601jgwhXR9BHGm1Lp5eSUE50GBFkFwXiobNJUoTfc12KXHccMt\n"
    "kwngjvPMIEx+Cg1yMz9P7fdj5dPBHR3CjvO7lGot+mGZHEgMxhnWJLcg1adSLc96\n"
    "NeklvhTk98A/NUwz0pqqW82+B8h+usxLEYS6HwOVAoGBAI61vtPMSconrpcPlefR\n"
    "19ifVXtEWUjCZxebAFEAP3qbUTn6irGftqq/ZeAdyj5I0LWGkwh5c+gkyhVNmGtp\n"
    "zNjwBGzgDSAqnDSVxtuJAVKX9STY/C7c33ljOc4R/Pcus/BbDU+UQU+S6H0Cp0Ba\n"
    "XsNO57TuwOteB2pbd7wGirjA\n"
    "-----END PRIVATE KEY-----\n"
;

static const char HL_TLS_CA2_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNzCCAh+gAwIBAgIUQlRH/OChg396IFUivtigDqT467IwDQYJKoZIhvcNAQEL\n"
    "BQAwKjEoMCYGA1UEAwwfSHVsbCBTTVRQIFRlc3QgQ0EgMiAodW5yZWxhdGVkKTAg\n"
    "Fw0yNjA5MDIxNzEwMzRaGA8yMDU2MDgyNTE3MTAzNFowKjEoMCYGA1UEAwwfSHVs\n"
    "bCBTTVRQIFRlc3QgQ0EgMiAodW5yZWxhdGVkKTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
    "ggEPADCCAQoCggEBAJIAmW9YT69EUXufJpjs4Q9imxTULIeVteSIF0peVrwA0VdM\n"
    "hGQSWV7yn221X7Ky8WbzQVwsZ1ZzRGnVTmPM2+wRUmCldAVc1qDOL1lVomYjJQJl\n"
    "d7rU5lOa1a2L3rHbPKKN45n1Jaa2835fwbMLDCiNXeteM272+edRl+vgw0xOmGM+\n"
    "dpXeDnnvNH2skHjsrUYi9uf9Z0QC33c/+4KmrT5LeV1wF99izt+iYhBmiiJTxdyC\n"
    "53kNZQ6dSKvjyj78m4/g4LiI7bT1ueHEmSla/ei+ztkjAV/tLCFA+2Q8Rfkvx6FG\n"
    "/RLh+nyQkV6UWn+ajqt3LilFZ941zcmZYWp92xkCAwEAAaNTMFEwHQYDVR0OBBYE\n"
    "FGHEpUdit7je+jhNlQNdhPRdGDCXMB8GA1UdIwQYMBaAFGHEpUdit7je+jhNlQNd\n"
    "hPRdGDCXMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAAJrFOct\n"
    "x7SaNkSuYVr94gpNYv8WpvUgWSFmNnPRqr3oHuk46CttXShJsPmTc63xdun8AxPw\n"
    "fV+EkVM1cPN+jUU+GPUJO8EUaalXoEJ/QUiSpKm9dBZzibkQmCnkEY5d7U4DZAHF\n"
    "GsEVFCnjH0sZTLksxNG4DxZPk6EjghE8Hs7osIou0b4CpkbyhOvc6NQgnVjAKccr\n"
    "29A7qVfILNpcYM3rrjt7Cxkkb2kglLTsgE+139c4QJ/NbaOdtjCEzTGKZ8bXjRgs\n"
    "EjpKYhmunAXMMq1dwbjVd+7UgcbLVLri7hzHhdhoV4Tx7zPHewlnTs7o982D5iuz\n"
    "kjM3melzfgt0UYY=\n"
    "-----END CERTIFICATE-----\n"
;

#endif /* HL_SMTP_TLS_TEST_CERTS_H */
