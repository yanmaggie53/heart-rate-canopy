# Getting a Pulsoid access token

Pulsoid's own "Create new token" page (pulsoid.net/ui/keys) only works on the paid BRO plan. The free route is the authorization link that open-source Pulsoid integrations use:

https://pulsoid.net/oauth2/authorize?response_type=token&client_id=0025a50e-9449-4aa5-9c68-36d2903cb6a5&redirect_uri=&scope=data%3Aheart_rate%3Aread&state=&response_mode=web_page

1. Open the link, log in with the same Pulsoid account as the watch app.
2. Click Authorize.
3. Pulsoid shows the token on the page. Copy it.

The token only allows reading heart rate. It is long-lived, so keep it somewhere private and never commit it to this repo.

Websocket the prototype connects to:

    wss://dev.pulsoid.net/api/v1/data/real_time?access_token=TOKEN
