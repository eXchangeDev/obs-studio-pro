# OBS Pro platform-session live test

This credentialed test verifies that a local 50 Mbps YouTube Program feeds both YouTube and the replay buffer while Twitch Enhanced Broadcasting runs as an independent session.

## Preconditions

- Use the Win64 portable artifact built from this branch with a copied test profile.
- Keep Twitch as the native `Stream1` service for this increment and enable Enhanced Broadcasting in OBS' Stream settings.
- Create a private or unlisted YouTube broadcast and have its RTMPS stream key ready. Independent native YouTube authentication/docks remain follow-up work; this test uses the additional destination's OBS service settings.
- Enable the replay buffer in the normal Simple or Advanced Output settings.

## Configure the graph

1. In **Settings → Output → Additional encoded programs**, add a Program named `YouTube HQ`.
2. Bind it to the horizontal canvas, choose the intended video/audio encoders, and set the video encoder to CBR `50000 Kbps`.
3. In **Settings → Stream**, add a YouTube destination, keep it in an **Independent session**, assign it to `YouTube HQ`, and enter the YouTube RTMPS service settings.
4. In the native primary Stream tab, keep the session active, select Twitch and enable Enhanced Broadcasting. Select the optional vertical canvas if required.
5. In the normal **Settings → Output → Replay Buffer** section, set **Encoded Program** to `YouTube HQ`.
6. Apply settings and restart outputs if OBS asks.

After applying, the selected Stream, Program, and Canvas tabs must remain selected. The primary stream may be disabled; in that configuration **Start Streaming** starts only the other active sessions.

## Exercise the lifecycle

1. Click **Start Streaming**. Twitch Enhanced and the YouTube destination should start independently.
2. Start the replay buffer if it is not configured to start automatically.
3. Confirm Twitch and YouTube receive video, then save a replay clip.
4. Open Settings and use **Stop Session** on the YouTube destination. Twitch and replay must continue.
5. Use **Start Session** on YouTube. It must reconnect without rebuilding the Twitch multitrack encoder group.
6. Use **Stop Session** on the primary `stream1` session. YouTube and replay must continue.
7. Stop all remaining outputs, then repeat the full start/stop cycle to catch stale callbacks.

## Verify the replay

Inspect the saved clip with `ffprobe` or MediaInfo. Its codec, resolution, frame rate, and audio mix must match `YouTube HQ`. A short clip's measured average bitrate may be below 50 Mbps even when its encoder was configured for 50,000 Kbps, so verify the OBS log's Program binding and encoder settings as well.

## Expected isolation

- A YouTube transport error only changes the YouTube session state.
- A Twitch multitrack error only changes the `stream1` session state.
- Stopping one session never invokes a stop on the other session or on replay.
- Dynamic bitrate is enabled only when one session owns the Program encoder, or when its endpoints use sequential failover. Parallel sessions never share a dynamic-bitrate controller.
- Logs and serialized diagnostic data must not print raw stream keys or passwords.

## Deterministic lifecycle test

Configure with `-DENABLE_OBS_PRO_TESTS=ON`, build the two `obs-pro-platform-session-*` targets, and run CTest. The lifecycle scenario test performs the same state sequence without network credentials and verifies that the saved replay resolves to the `YouTube HQ` Program with CBR `50000 Kbps`. The credentialed procedure above remains required to validate real provider transport, authentication, and the produced media file.
