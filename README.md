# ColdChain 

## Credits
- **olikraus** - [u8g2](https://github.com/olikraus/u8g2) OLED graphics library
- **nopnop2002** - [esp-idf-ssd1306](https://github.com/nopnop2002/esp-idf-ssd1306) SSD1306 driver for ESP-IDF

## Web server (simulated sensors, login, inventory, QR)
Real sensors are removed; `server/` simulates temperature, door, power, compressor and heat budget with realistic random changes.
```
cd server && npm install && npm start      # http://localhost:3000
```
Demo users: `admin/admin123`, `pharma/pharma123`, `inspector/inspect123` (change them in `data.json` / `server.js`).
Set `PUBLIC_URL=http://<your-ip>:3000` so QR codes open from phones. Set `SECRET` for stable login sessions across restarts.

| Permission | Admin | Pharmacist | Inspector |
|---|---|---|---|
| Dashboard | ✔ | ✔ | ✔ |
| View inventory | ✔ | ✔ | ✔ |
| Add batch / move fridge | ✔ | ✔ | ✘ |
| Delete batch, users, simulation controls | ✔ | ✘ | ✘ |
| Audit log | ✔ | ✘ | ✔ |

All checks run on the server (HMAC-signed HttpOnly cookie, scrypt passwords). Public QR page `/b/<batch-id>` needs no login and shows only vaccine, expiry and Safe/At Risk/Discard.
