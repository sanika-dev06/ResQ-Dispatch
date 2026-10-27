# ResQ Dispatch - C++ backend (Crow + SQLite)

Your existing frontend (HTML/CSS/JS) now talks to a C++17 backend through a REST API.
The UI markup is **unchanged**; only the JavaScript data layer was switched from
`localStorage` to API calls.

```
ResQ-Backend/
  CMakeLists.txt, vcpkg.json     build files (dependencies: Crow, SQLite3)
  CMakePresets.json, .vscode/    VS Code build/run/debug setup
  setup.bat build.bat run.bat test.bat   Windows helpers
  db/schema.sql                  tables
  db/seed.sql                    road graph for Dijkstra (nodes + edges)
  db/demo_data.sql               demo fleet + 4 sample emergencies (also used by "Reset Records")
  src/main.cpp                   Crow HTTP layer: routes only
  src/service.hpp                all business logic (auth, notifications, queue, dispatch, vehicles)
  src/pqueue.hpp                 priority queue (indexed binary heap)
  src/graph.hpp                  road graph + Dijkstra's algorithm
  src/paths.hpp                  finds the project folder from any start location
  src/crypto.hpp                 SHA-256, HMAC, PBKDF2 password hashing, secure random
  src/db.hpp, src/jsonw.hpp      SQLite wrapper, JSON writer
  public/index.html, app.js      your frontend (served by the backend)
  tests/                         automated tests
```

## 1. Build and run on Windows (VS Code)

**Install once:** [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) (or Community)
with the **"Desktop development with C++"** workload, [CMake](https://cmake.org/download/) (3.21+),
[Git](https://git-scm.com/download/win), and VS Code with the **C/C++** and **CMake Tools** extensions
(VS Code will offer them when you open the folder).

**Then:**

1. Extract the zip, then in VS Code: **File > Open Folder...** and choose the extracted folder
   (the one that contains `CMakeLists.txt`).
2. Press **Ctrl+Shift+B** (task *ResQ: Build (Release)*). The first run installs vcpkg to `C:\vcpkg` if it
   is missing, then downloads and compiles Crow + SQLite - allow **5-10 minutes**. Later builds take seconds.
3. **Terminal > Run Task... > ResQ: Run server**, then open **http://localhost:8080**.
4. Sign in with `admin` / `ResQ@123`.

Prefer the command line? In the project folder run `build.bat`, then `run.bat`
(`test.bat` runs the automated tests; `setup.bat` only installs vcpkg).
Debugging: **F5** (*ResQ: Debug server*) builds a Debug version and starts it under the debugger.

Notes:
- Do not open `index.html` by double-clicking; the page must be served by the backend.
- The server finds its `db/` and `public/` folders by itself, so it works from any start folder.
- The database file `resq.db` is created next to `CMakeLists.txt` on first start. To start from
  scratch, stop the server and delete `resq.db*`.
- If setup.bat just installed vcpkg while VS Code was open, close and reopen VS Code once.
- If port 8080 is busy: `set RESQ_PORT=8081` before `run.bat`, and open that port instead.

**Default login:** username `admin`, password `ResQ@123` (the same as your old demo).
Change it before showing the project to anyone: `set RESQ_ADMIN_PASSWORD=YourNewPassword` *before the
very first start* (the password is only applied when the admin account is first created), or later
through `PUT /api/profile/password`.

| Environment variable | Default | Meaning |
|---|---|---|
| `RESQ_PORT` | `8080` | HTTP port |
| `RESQ_BIND` | `127.0.0.1` | Address to listen on (localhost only; `0.0.0.0` exposes it to the network) |
| `RESQ_DB` | `<project>/resq.db` | SQLite file |
| `RESQ_ADMIN_PASSWORD` | `ResQ@123` | Password for the first-created `admin` account |
| `RESQ_ROOT` | auto-detected | Folder containing `db/` and `public/` |

## 2. What the backend does

**Accounts and roles.** Two roles: `admin` (seeded) and `dispatcher` (everyone who signs up).
Passwords are stored as salted PBKDF2-HMAC-SHA256 (100,000 iterations); a login returns a random
bearer token, valid 12 hours, of which only the SHA-256 hash is stored. Five wrong passwords for the
same username lock that username for 60 seconds. Only admins can reset records, manage vehicles and
list users.

**Notifications.** Stored per user. New emergencies, dispatches, completed trips, vehicle changes and
profile edits create notifications for the relevant users. *Mark all as read* and *Clear all* are
server-side (`POST /api/notifications/read-all`, `DELETE /api/notifications`), so they survive a
refresh or a new login.

**Priority queue.** Waiting emergencies live in an in-memory binary min-heap ordered by
(priority, arrival id): High before Medium before Low, oldest first within a level. Insert and
remove are O(log n), peek is O(1). The heap is rebuilt from the database on start-up. The UI shows
the order the server computes.

**Dijkstra.** `db/seed.sql` defines 11 Pune locations and the road distances between them. The
emergency's free-text location (e.g. "MG Road, Pune") is matched to a node by name/alias. One
Dijkstra run from that node gives the distance to every available ambulance; the nearest *suitable*
one is chosen (ties go to the lower id). Suitability: *Cardiac Arrest* and *Breathing Issue* need an
**ALS** unit; every other type accepts ALS or BLS. If no suitable unit is free the dispatch is
refused (HTTP 409) instead of silently sending the wrong unit. If the location is not on the graph,
the first suitable unit is used and the response says so (`method: "fallback"`, no distance).
The distances in `seed.sql` are **rough illustrative values, not surveyed road distances** - edit the
`edges` table to change them (restart the server to reload).

Manual **Assign** on a specific ambulance still works as before; if that unit is unsuitable it is
allowed but the response contains a warning that the dispatch panel displays.

## 3. API reference

All endpoints return JSON. Errors look like `{"error":"message"}`. Except login, signup and health,
send `Authorization: Bearer <token>`.

| Method and path | Role | Body / notes |
|---|---|---|
| `POST /api/auth/login` | public | `{username, password}` (username or email) -> `{token, user}` |
| `POST /api/auth/signup` | public | `{name, username, email, password}` -> 201 `{token, user}` (role `dispatcher`) |
| `POST /api/auth/logout` | any | revokes the token |
| `GET /api/auth/me` | any | `{user}` |
| `PUT /api/profile` | any | `{name, email}` (email may be empty) |
| `PUT /api/profile/password` | any | `{currentPassword, newPassword}`; revokes all your sessions |
| `GET /api/notifications` | any | `{notifications:[...], unread}` (latest 30) |
| `POST /api/notifications/read-all` | any | marks all of your notifications read |
| `DELETE /api/notifications` | any | clears all of your notifications |
| `GET /api/state` | any | emergencies, fleet, history, `queue` (ids in dispatch order), `stats` |
| `POST /api/emergencies` | any | `{type, priority, location, patient, details}` -> 201 with `queuePosition` |
| `GET /api/emergencies/{id}/nearest` | any | preview which vehicle Dijkstra would pick (changes nothing) |
| `POST /api/dispatch` | any | `{requestId?, ambulanceId?}`; none = top of the queue + Dijkstra |
| `POST /api/dispatches/{id}/complete` | any | finishes a trip; the ambulance becomes Available at City Centre |
| `GET /api/vehicles` | any | list |
| `POST /api/vehicles` | admin | `{id, type: "ALS"/"BLS", location}` |
| `PUT /api/vehicles/{id}` | admin | any of `{type, location, status}`; status is Available, On Duty or Maintenance |
| `DELETE /api/vehicles/{id}` | admin | refused while on a trip or if it has dispatch history |
| `POST /api/admin/reset` | admin | restores the demo fleet/emergencies, clears dispatch history |
| `GET /api/users` | admin | account list (no password data) |
| `GET /api/health` | public | `{ok:true}` |

Typical status codes: 400 invalid input, 401 not signed in / wrong password, 403 not allowed for
your role, 404 not found, 409 conflict (e.g. no available ambulance, already dispatched),
429 too many failed logins.

## 4. What changed in the frontend

- `public/index.html`: lines 1-144 (all HTML and CSS) are **byte-for-byte your original file**; the
  big inline `<script>` is replaced by `<script src="/app.js">`.
- `public/app.js`: your original functions with the same names and behaviour; storage calls became API
  calls. Removed: the hard-coded `admin` password and the plain-text account list in the browser.
- Small additions: the ambulance list shows the unit type (`· ALS`); the dispatch panel shows
  the distance and route; the "total ambulances" labels follow the real fleet size; "x mins ago" is
  computed from real timestamps; the page refreshes from the server every 5 seconds so two dispatchers
  see each other's changes.
- Behaviour that is now different on purpose: *Reset Records* is admin-only (dispatchers get a
  message); automatic dispatch uses the ALS rule above; "Dispatched Today" starts from the
  baseline of 28 stored in the `meta` table (key `dispatched_baseline`; set it to `0` for a real count).
- Left alone because you asked not to touch the UI: the static text on the "DSA Concepts" page,
  the "Avg Response Time 3.5" card, and the notes saying data lives "in this browser session".

## 5. Test status - read this

Verified in the development sandbox (Linux, g++ 13, SQLite 3.45):

| Area | How it was checked | Result |
|---|---|---|
| SHA-256, HMAC, PBKDF2, hex, random | known-answer vectors (FIPS 180, RFC 4231, RFC 7914) | pass |
| Priority queue | unit tests + 500-step randomised comparison with a sorted list | pass |
| Dijkstra | textbook graph, unreachable and unknown nodes | pass |
| Service layer on real SQLite (auth, lockout, profile, notifications, queue, dispatch, Dijkstra choices, ALS rule, vehicles, roles, reset, restart persistence) | `tests/test_service.cpp`, 162 checks; 4 deliberately injected bugs were all caught | pass |
| `public/app.js` against the real service (login, signup, queue render, new emergency, dispatch, complete, Mark all as read, Clear all, profile, reset, logout, session resume, expired token, offline) | Node with a stub DOM + a harness calling the C++ service, 60 checks | pass |

| `src/main.cpp` start-up path (root detection from a foreign folder, DB creation, seeding, admin created once, restart reuses the DB, clear error for missing files) | built against a *stand-in* `crow.h` (listening is a no-op there) and run on real SQLite | pass |
| `src/main.cpp` routes: handler shapes and return types | compiled warning-free (`-Wall -Wextra -Wpedantic`) against that stand-in, which asserts every handler matches its `<int>`/`<string>` route | pass |

**Not verified:**
- The **real Crow** and the CMake/vcpkg/Visual Studio build were never run: the sandbox has no Crow,
  no network and no Windows. The stand-in only proves my own code is consistent with the Crow API as I
  know it (1.0+, what vcpkg installs); a difference in the real library, or an environment problem on
  your PC, can only show up when you build. If `build.bat` or the VS Code build task prints an error,
  send me the first error line.
- The UI has not been opened in a real browser against the running server.

## 6. Known limits

- Plain HTTP on localhost - fine for a college demo; put it behind HTTPS before any real use.
- The password hashing is hand-written (tested against the official vectors) to avoid extra
  dependencies; a production system should use a vetted library such as libsodium or bcrypt.
- One SQLite connection guarded by one mutex: ample for a demo, not for heavy load.
- Road distances are illustrative; this is a simulation and contacts no real ambulance or service.
