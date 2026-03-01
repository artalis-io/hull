-- wrk script: POST with a small JSON body
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"message":"hello","value":42}'
