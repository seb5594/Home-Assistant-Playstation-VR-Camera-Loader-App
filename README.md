# Example App

<!-- badges:begin (generated from project metadata) -->
[![version](https://img.shields.io/static/v1?label=version&message=0.1.0&color=1877A5&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/releases) [![released](https://img.shields.io/github/release-date-pre/seb5594/Home-Assistant-App-Template?label=released&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/releases) [![build](https://img.shields.io/github/actions/workflow/status/seb5594/Home-Assistant-App-Template/ci.yml?branch=main&label=build&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/actions/workflows/ci.yml)<br>
[![stars](https://img.shields.io/github/stars/seb5594/Home-Assistant-App-Template?label=stars&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/stargazers) [![issues](https://img.shields.io/github/issues/seb5594/Home-Assistant-App-Template?label=issues&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/issues) [![updated](https://img.shields.io/github/last-commit/seb5594/Home-Assistant-App-Template?label=updated&style=flat)](https://github.com/seb5594/Home-Assistant-App-Template/commits/main) ![image pulls](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseb5594%2FHome-Assistant-App-Template%2Fbadges%2Fimage-pulls.json&style=flat)<br>
![arch](https://img.shields.io/static/v1?label=arch&message=amd64%20%C2%B7%20aarch64&color=157F71&style=flat)<br>
![mounts (read-only)](https://img.shields.io/static/v1?label=mounts%20%28read-only%29&message=config&color=157F71&style=flat)<br>
![stage](https://img.shields.io/static/v1?label=stage&message=stable&color=2F855A&style=flat) ![options](https://img.shields.io/static/v1?label=options&message=1&color=1877A5&style=flat)<br>
![image size](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseb5594%2FHome-Assistant-App-Template%2Fbadges%2Fimage-size.json&style=flat) ![runtime tests](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseb5594%2FHome-Assistant-App-Template%2Fbadges%2Fruntime-tests.json&style=flat) ![last build](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fseb5594%2FHome-Assistant-App-Template%2Fbadges%2Flast-build.json&style=flat)<br>
[![Home Assistant](https://img.shields.io/static/v1?label=Home%20Assistant&message=Add%20app%20repository&color=18BCF2&style=flat&logo=homeassistant&logoColor=white)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fseb5594%2FHome-Assistant-Apps)
<!-- badges:end -->

A starting point for a Home Assistant app with the shared pipeline already wired in: validation,
multi-architecture build, runtime tests, GHCR release and README badges.

## Using this template

1. Create a repository from this template. Name it `Home-Assistant-<project>-App` so the
   [app catalog](https://github.com/seb5594/Home-Assistant-Apps) picks it up.
2. Rename `example-app/` and set `slug`, `name`, `url`, `description` and the options in `config.yaml`.
   The folder name must equal the slug.
3. Replace the repository URL in the `Dockerfile` label and change the greeting logic in `run.sh`.
4. Adjust `.github/tests/runtime.sh`; it runs inside the built image.
5. Regenerate the badge block above and commit the result:

   ```bash
   git clone https://github.com/seb5594/hass-workflows .engine   # private; needs access
   python .engine/.scripts/readme.py
   ```

6. Raise `version` in `config.yaml` and the `BUILD_VERSION` default in the `Dockerfile` together
   with a new `CHANGELOG.md` entry for every release.

The only workflow, `.github/workflows/ci.yml`, calls the shared engine at a pinned commit and
needs the `ORCHESTRATION_PAT` secret. Pin base images and every `apk` package to an exact
version; the pipeline rejects anything floating.

<!-- support:begin (generated, shared text) -->
## Say thanks

All my projects are built in my spare time, just for the fun of it. Still, they take a lot of time and care, and many of the smart home gadgets I buy end up here because I want to make them cloud-free and smart-home ready. Every bit of support means a lot to me.

If my work helped you or turned out to be useful, show your appreciation with a hot drink for my next sleepless night.

[![Buy Me a Coffee](https://img.shields.io/static/v1?label=Support&message=Buy%20Me%20a%20Coffee&color=FFDD00&logo=buy-me-a-coffee&logoColor=black&style=for-the-badge)](https://buymeacoffee.com/seb5594) [![PayPal](https://img.shields.io/static/v1?label=Support&message=PayPal&color=0070BA&logo=paypal&logoColor=white&style=for-the-badge)](https://www.paypal.com/donate/?hosted_button_id=QMQPNRENXDN26)
<!-- support:end -->
