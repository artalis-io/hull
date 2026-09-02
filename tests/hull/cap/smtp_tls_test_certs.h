/**
 * @file smtp_tls_test_certs.h
 * @brief FIXED test-only TLS material for the in-process mbedTLS SMTP peer.
 *
 * TEST-ONLY. Minted offline once (RSA-2048, ~30y validity) and embedded as PEM
 * literals so the TLS suite has NO external openssl process and NO network
 * dependency. NEVER compiled into a shipped binary - included only by
 * tests/hull/cap/test_smtp_transport.c and test_smtp_e2e.c.
 *
 *   HL_TLS_CA1_PEM  - CA that signed the server cert (client trusts this = success)
 *   HL_TLS_SRV_CRT  - server leaf, CN=localhost, SAN DNS:localhost (signed by CA1)
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
    "MIIDHzCCAgegAwIBAgIUA8cxhsqm711fSFQX9hsaOTdGYKIwDQYJKoZIhvcNAQEL\n"
    "BQAwHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3QgQ0EgMTAgFw0yNjA5MDIxNzAz\n"
    "MjVaGA8yMDU2MDgyNTE3MDMyNVowHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3Qg\n"
    "Q0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALzWT2yKzx314Gcc\n"
    "b2lg1/Qs7TT26AipRsZVyzfgYn5lOXNn58Y2p2C6GkPVwrJe53HuBUZNSxPIxymx\n"
    "ZmJDFPFRRLIOJWs37lvj68yFdSPm6l6h0BdErSdyCrdEtDGkIx4u0AkvGWf1cYwn\n"
    "GEWnRpFVskoVoQCeLHIP5PoVuWVGyvhl67HY9AJ4KcszZS8b6+6lNVAbBH6X3o3k\n"
    "gsvVktjsBVTJj2PUkfJGgOpHSOgmLryGRhMIOkvq72tA6wjJhp8L/BmE596OglPR\n"
    "J2LjIvE+8K8rKyXvEg5y2OyqQECphE5kh5pPWaoBawmUwjaPZgp9z0dDuVccyBxl\n"
    "FP425Z0CAwEAAaNTMFEwHQYDVR0OBBYEFCTTv49kkxNu1DzUvoYL04z5ktUSMB8G\n"
    "A1UdIwQYMBaAFCTTv49kkxNu1DzUvoYL04z5ktUSMA8GA1UdEwEB/wQFMAMBAf8w\n"
    "DQYJKoZIhvcNAQELBQADggEBAFYQe66Q3CSK0gSpcmdDVol9jJPQ2BjdXAFsAawx\n"
    "D2beo2uG+6VT+n5lyqKzuE+xQLFguff5QWq+P8MtzKfXfJ74bllRWghwZOurmk7u\n"
    "k4WJMm5Dx3dUPqz3uvGAYnK5vOz0oRsY8a521i+gGkE1URSjjc4fgb9uMkPM8rzY\n"
    "EPMYSp4lJKjvHCm/0xjyuJRqMMiwPlUpDPnG6fSkBhlb/7cWZhACpThBCSx+ph8j\n"
    "pbffL0EB2G6N8YUZZLSskvqT88HtuobehoL62hadfdrCu6Ki6HL/hAS8n6vCEIps\n"
    "tnDOeBU2cvDNZuJ2UjaHNqh59qn11bRmKAqoE/Am28DmUH8=\n"
    "-----END CERTIFICATE-----\n"
;

static const char HL_TLS_SRV_CRT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDJTCCAg2gAwIBAgIUYRmZGBFQHR2WglwBDEL8nkHX9FMwDQYJKoZIhvcNAQEL\n"
    "BQAwHjEcMBoGA1UEAwwTSHVsbCBTTVRQIFRlc3QgQ0EgMTAgFw0yNjA5MDIxNzAz\n"
    "MjVaGA8yMDU2MDgyNTE3MDMyNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjAN\n"
    "BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvCIdlMtwvSfUqbm0G300ByjHENdA\n"
    "FS6Snr/ePnBandsKZYAEenWFGtP0SUFT0QNw13kNZJb5M34T0KoDc9QdrD6L+A45\n"
    "4ENQlvobtjRxkUZ7V4EK+JsUGF3n3IZ0W5lD79cXMb8JIoZZY18OVfh9BA1WF09E\n"
    "UBzKzTFZ0DYHFIfPGc2opcyYeRbyoLHYaFovczf5BrXs5/ieDpZ3ZYvqVx5NRHq4\n"
    "1JAQ6Oyw/X4+Xfs7KYgJXXXqTwiqRo1RGK6LETBArhYMa392w0nXxx56qBxC/tMX\n"
    "UFbJU3Nb97SSP3FyeIOzRSxrqvJ12/NnYvDthEFYpm8lEehX6jXHtdymywIDAQAB\n"
    "o2MwYTAUBgNVHREEDTALgglsb2NhbGhvc3QwCQYDVR0TBAIwADAdBgNVHQ4EFgQU\n"
    "naOFHj83S7w9gtZ+wQC8jTFcsq8wHwYDVR0jBBgwFoAUJNO/j2STE27UPNS+hgvT\n"
    "jPmS1RIwDQYJKoZIhvcNAQELBQADggEBAEt3WiAdcco/lAQ9gg47TURbPaCs1ftD\n"
    "jUMncZxwTB57QIUjGBTZ+BwTOFrrdYj3Cc/vzrTVynozKHUygNmUfUwULD0C1PET\n"
    "n5fvCugRC7RGiZ3eV6QAqJaUWGFHUHJKJOBcUEdWO4IN/L/pVAseqFmTzymg8KzS\n"
    "qoO1Qa3s2Tqiflkjrwsm6wyETalsmh+XCIPDq7azbfzxE5Dk28eFVM4Hd/hnw5tA\n"
    "Z4yIktHJ5EpAP3YISJRSPZ4UaTeaiHk4L0Str36NVQFOiA4ZI7SC4UZ7jtJw3/q+\n"
    "p+PxSsi0sIYAYVAEbmFrPvWjyxhDp8tPZY+bgPzp6Wzwt708slySVSw=\n"
    "-----END CERTIFICATE-----\n"
;

static const char HL_TLS_SRV_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC8Ih2Uy3C9J9Sp\n"
    "ubQbfTQHKMcQ10AVLpKev94+cFqd2wplgAR6dYUa0/RJQVPRA3DXeQ1klvkzfhPQ\n"
    "qgNz1B2sPov4DjngQ1CW+hu2NHGRRntXgQr4mxQYXefchnRbmUPv1xcxvwkihllj\n"
    "Xw5V+H0EDVYXT0RQHMrNMVnQNgcUh88ZzailzJh5FvKgsdhoWi9zN/kGtezn+J4O\n"
    "lndli+pXHk1EerjUkBDo7LD9fj5d+zspiAlddepPCKpGjVEYrosRMECuFgxrf3bD\n"
    "SdfHHnqoHEL+0xdQVslTc1v3tJI/cXJ4g7NFLGuq8nXb82di8O2EQVimbyUR6Ffq\n"
    "Nce13KbLAgMBAAECggEAA/YElmyUHT9t8FGLHmW2nIykvaYyi1+gJvyA2DgIXbjO\n"
    "EWYDx3TwTlN2b2Zoi9wyhNmKc9A5Q0wdFwXFWIEiS3fuUtTbLhgXZckH9VWfAIEx\n"
    "4/KWbGE40pcuObaQ/kQN/0o8NZ1r5VCxKBueaHv4sdxQBYyd3RabbyeMrbVoKmv1\n"
    "Adi3A3L8rT8kvsrk9wi0v3LR2i6HZF5kRvEbz0B4TLId2HAw56APcwNRjG78KBI0\n"
    "DhmWH5WM2D/s9h/0r4zHiX6rcq8R5QRPTBX98Rj3p0adLGwdUjgzQ/WbY/biAEcZ\n"
    "H3JkbNDEf/JZ5AsDjFDklRA76tsAM/E2U4/B2IjOaQKBgQDhhM5RLAINWUO1o/8r\n"
    "vIMEL0qqkrnL6PDq2kOjbMG5Tbo3QMkDlf1bJRh3XTRP91pSDhuDZBCdDWPUN8nF\n"
    "9AWmt5BozA/GGpyYP+t9sDLPf/o3xZuM5PlU+DLv+kwApfhfl9+tiM6e8+srlnVv\n"
    "jE9zSrYImm1uX9NqOZOPxYShyQKBgQDVj7r//75WRFCoY8PB3aeHQmY3SuGVnt+B\n"
    "e37QUWvPK4NXEDXtTvfPUtKbEz/FspkYTJAib9L+cYWwmix+5a3K2cl5cc6CKgLA\n"
    "wUglIphJzSSWvC2plpbo3tDahXmFK48jqNr2g8ZpK2IbKqONSH69jJpQL1WX2LAL\n"
    "E5klDbDt8wKBgBAuFRdhzuviQadgksgyiDveoL7INChbGB3hdwTcorGG0BtyvHlT\n"
    "Y5AMg0rdFwm9t283r+WnYkHCWi05q3JWZalmdifurBsMgbuyqlSkNaEJj4w800Iy\n"
    "k2jzPcRV8uoA/mbtYJD0xc5FtdO0wcw1BuZAr/rCCaPnoCV46AtiondxAoGBAMVx\n"
    "uDMckC4TxPqaGg9vzYZpJjWynnOFSiDdO3aAAIuuMCTbUPBRFR1x9lL3bftqzs4/\n"
    "YvbjqhAihrarI75CvPYReos0Y/fFvXvWdswWevOonU5bNmBXYLh14GRYCOzNQ+51\n"
    "G9PUKylqua5iMonZ34uBpd3ClYDpDoF/IhS23sBvAoGALeAMijc+xh4dph8xKjQ3\n"
    "FJOtiJT4RwiQegZ3dtHn4CYtotNGhKZ2eXHetOC9rhfyJ96rlsKpQ7C971T6fBo+\n"
    "a1V4KRsnxl5xfrByOmP78Ta01xxZ6iAhgs6vSNotOOX3g8sMf+3/4dYKheEW8fUl\n"
    "8eWZacSUJ/MkEGwMSOnlybk=\n"
    "-----END PRIVATE KEY-----\n"
;

static const char HL_TLS_CA2_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNzCCAh+gAwIBAgIUCn6cE+n144Eoho3j36zT9tAD8JEwDQYJKoZIhvcNAQEL\n"
    "BQAwKjEoMCYGA1UEAwwfSHVsbCBTTVRQIFRlc3QgQ0EgMiAodW5yZWxhdGVkKTAg\n"
    "Fw0yNjA5MDIxNzAzMjVaGA8yMDU2MDgyNTE3MDMyNVowKjEoMCYGA1UEAwwfSHVs\n"
    "bCBTTVRQIFRlc3QgQ0EgMiAodW5yZWxhdGVkKTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
    "ggEPADCCAQoCggEBAMPvIK5fLdlqYPatgsdEUEYX7tEN2LPwIr6DslAbKXYCmkk9\n"
    "hj0m6HYMGVHo6IVYy1KQUxFMWE3+v4NchgykmwgNsRuF7GAZRZKZ8vYqNDaLmV9S\n"
    "qrvKBSweobzH4I+q1Jf0eIt4powOxau2XPlHlUjBhDLukhiq7zvMeSMW6IUx/94v\n"
    "BxhRZJ0PrL35wlDbsTITbVpCdtXWDOngiVRJZJlsOxXesn/X/aZgkMAREaA36Fow\n"
    "Q9NcjT3HyIGv/OLbV0BHA7wqjl4Fadpo9oNWyzSwDY71VMuRZ+FP1xt1bfp5T3v4\n"
    "kxhJLDJ4G3kUGS17r16BavZEF3tOorCThLvqAT8CAwEAAaNTMFEwHQYDVR0OBBYE\n"
    "FBWvIRWCJXvernnWg8JLH6BFcW78MB8GA1UdIwQYMBaAFBWvIRWCJXvernnWg8JL\n"
    "H6BFcW78MA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAE95TM66\n"
    "0RgE2MDTv7nu9FiWC2yeOCDZsqX4rG5xqFGpPwIYIn4Y5ciXk71uH1Psbl0Xegff\n"
    "VMSN679OptL4ryKbXNWw4VMZC6rA5ZkgIoIabtNWgppg3U0maXM9IG5QhpCKzguf\n"
    "lbrkEGFCYrI8lkNH7s3XyEI5Ophy+VGyc4LHvvaxZeQOGpsoZn7v0M+lQNhHJj4t\n"
    "vCi/+C3KjT6xGQOgCWYF6LxoBnsOCJRy9LE49S+5a6T2iOv/lmmHgbxUn4dHA0hE\n"
    "cgTS6TEBU/E7i5RoOwhilLgFrPveaRldLq1IWpM6j+lSe/OQdO4PqbINiIbGpqya\n"
    "iJUq0IpXwUOw3YY=\n"
    "-----END CERTIFICATE-----\n"
;

#endif /* HL_SMTP_TLS_TEST_CERTS_H */
