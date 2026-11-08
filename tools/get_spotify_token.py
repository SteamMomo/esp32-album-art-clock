import base64
import hashlib
import hmac
import json
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer


REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPE = "user-read-currently-playing"
callback_result = {}


class CallbackHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        state = params.get("state", [""])[0]
        if parsed.path != "/callback" or not hmac.compare_digest(state, self.server.oauth_state):
            callback_result["error"] = "Invalid OAuth callback state or URL."
            self._reply("Login failed. Close this tab and run the helper again.", 400)
            return

        if "error" in params:
            callback_result["error"] = params["error"][0]
            self._reply("Spotify authorization was not granted. You can close this tab.", 400)
            return

        callback_result["code"] = params.get("code", [""])[0]
        self._reply("Spotify connected. You can close this tab and return to the terminal.")

    def _reply(self, message, status=200):
        body = (
            "<!doctype html><meta charset=utf-8>"
            "<title>Spotify authorization</title>"
            "<p>" + message + "</p>"
        ).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format, *_args):
        pass


def main():
    client_id = input("Spotify app client ID: ").strip()
    if not client_id:
        raise SystemExit("A client ID is required.")

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode("ascii")).digest()
    ).rstrip(b"=").decode("ascii")
    oauth_state = secrets.token_urlsafe(24)

    server = HTTPServer(("127.0.0.1", 8888), CallbackHandler)
    server.oauth_state = oauth_state
    server.timeout = 1
    authorization_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(
        {
            "response_type": "code",
            "client_id": client_id,
            "scope": SCOPE,
            "redirect_uri": REDIRECT_URI,
            "state": oauth_state,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
        }
    )

    print("Opening Spotify login. If it does not open, visit this URL:")
    print(authorization_url)
    webbrowser.open(authorization_url)
    print("Waiting for authorization (up to 4 minutes)...")

    deadline = time.monotonic() + 240
    try:
        while time.monotonic() < deadline and not callback_result:
            server.handle_request()
    finally:
        server.server_close()

    if "error" in callback_result:
        raise SystemExit("Spotify authorization failed: " + callback_result["error"])
    code = callback_result.get("code", "")
    if not code:
        raise SystemExit("Authorization timed out. Run the helper again.")

    request_data = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
            "code_verifier": verifier,
        }
    ).encode("ascii")
    request = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=request_data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            token_response = json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise SystemExit(
            "Spotify token exchange failed (HTTP "
            + str(error.code)
            + "). Check the app's client ID and redirect URI."
        ) from error

    refresh_token = token_response.get("refresh_token")
    if not refresh_token:
        raise SystemExit("Spotify did not return a refresh token.")

    print("\nAdd these values to src/secrets.h:")
    print('#define SPOTIFY_CLIENT_ID "' + client_id + '"')
    print('#define SPOTIFY_REFRESH_TOKEN "' + refresh_token + '"')


if __name__ == "__main__":
    main()
