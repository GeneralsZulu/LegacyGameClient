# Code Signing (Azure Artifact Signing)

Every PE that ships is Authenticode-signed: the installer, the uninstaller
embedded in it, `generalszh_zulu.exe` and `ZuluLauncher.exe`. Signing runs
from Linux through [jsign](https://ebourg.github.io/jsign/) against Azure
Artifact Signing (formerly Trusted Signing), so no Windows box and no
hardware token are involved.

Unsigned builds make Windows SmartScreen show "Windows protected your PC"
for every new release, because reputation is tied to the file hash and
every release is a new hash. With a signed installer the reputation attaches
to the publisher identity instead and carries over between releases.

## How it is wired

- `scripts/sign.sh FILE...` does the signing. It is a no-op that prints one
  warning when signing is not configured, so plain `make installer` and dev
  builds keep working on any machine.
- The Makefile signs the staged game exe and launcher right after copying
  them out of the docker build dir, then passes the script to NSIS as
  `SIGN_CMD`.
- `installer/Zulu.nsi` signs the uninstaller via `!uninstfinalize` (the only
  moment the bare uninstaller file exists) and the finished installer via
  `!finalize`. Both fail the build on a non-zero exit.
- `make installer-release` exports `ZULU_SIGN_REQUIRED=1`, so a release
  refuses to build unsigned. `make installer-release ZULU_SIGN=0` is the
  explicit escape hatch and prints a loud warning.
- `make sign-status` shows the resolved config and whether java, az and the
  jsign jar are in place. The jar is downloaded on first use into
  `build/jsign/` and checked against a pinned sha256.

Artifact Signing certificates are valid for three days, so every signature is
countersigned by Microsoft's timestamp authority; that is what keeps the
signature valid after the certificate expires.

## One-time setup

The service was called "Trusted Signing" during preview; it went GA in 2026
as **Azure Artifact Signing**. Portal names below are the GA ones. The
signing endpoints (`<region>.codesigning.azure.net`), the resource provider
and the jsign flags did not change with the rename.

Azure side, all in the [portal](https://portal.azure.com):

1. Sign in with a Microsoft account and create a **Pay-As-You-Go**
   subscription. Free, trial and sponsored subscriptions are refused by the
   service. Basic SKU is a flat monthly fee (about $10, not pro-rated) for
   5000 signatures a month.
2. Subscription > Resource providers > register `Microsoft.CodeSigning`.
3. Before anything else, open the subscription's **billing account** and
   make sure the account type is "Individual" and the legal name and address
   match your government ID exactly. Individual identity validation reads
   its details from the billing account and the form is read-only; the
   validated name becomes the publisher Windows shows. Only the city, state
   and country from the address appear on the certificate.
4. Search for **Artifact Signing Accounts** > Create. Basic SKU. Region
   East US keeps the script's default endpoint; any other region needs
   `ZULU_SIGN_ENDPOINT` set to that region's host (wus2, cus, weu, ...).
5. On the new account, **Access control (IAM)** > Add role assignment.
   Assign yourself both **Artifact Signing Identity Verifier** (without it
   the New Identity button is greyed out) and **Artifact Signing
   Certificate Profile Signer** (what `az account get-access-token` needs
   to sign).
6. **Identity validations** > switch the dropdown to **Individual** > New
   Identity > Public. Pick the billing account, check the preview, Create.
   When the status flips to "Action Required", follow the link: it runs
   through AU10TIX (email PIN, phone, photograph your driver's license or
   passport plus a face check, all from the phone) and then lands a
   Verified ID in the Microsoft Authenticator app, which you present back
   to the portal. People report this finishing in under an hour. Individual
   validation is only offered for US and Canada residents.
7. **Certificate profiles** > Create > **Public Trust**, pick the validated
   identity, name it (for example `zulu-release`). Program Type stays None.

Build machine side:

```
sudo pacman -S azure-cli        # or the distro equivalent
az login                        # opens a browser once; tokens refresh on their own
mkdir -p ~/.config/zulu
cat > ~/.config/zulu/signing.env <<'CFG'
ZULU_SIGN_ACCOUNT=<artifact signing account name>
ZULU_SIGN_PROFILE=<certificate profile name>
ZULU_SIGN_ENDPOINT=eus.codesigning.azure.net
CFG
make sign-status
```

Then sign something throwaway to prove the chain end to end:

```
cp build/docker-vc6/launcher/ZuluLauncher.exe /tmp/probe.exe
scripts/sign.sh /tmp/probe.exe
```

Copy `/tmp/probe.exe` to a Windows machine, open Properties > Digital
Signatures, and confirm the signer name and a timestamp. After that, the
next `make installer-release` ships signed.

## Notes

- Basic SKU quota is 5000 signatures a month. A release uses four.
- Identity validation expires and has to be renewed; Microsoft emails
  reminders starting 60 days out. If it lapses, certificate renewal stops
  and signing fails until a new validation is linked to the profile.
- Artifact Signing does not issue EV certificates and Microsoft's own FAQ
  says SmartScreen reputation still accrues over downloads. Expect the
  warning to fade over the first releases rather than vanish on the first
  one; if it lingers, submit the signed installer at
  https://www.microsoft.com/wdsi/filesubmission.
- `az login` sessions last about 90 days. When `make sign-status` reports
  "NOT logged in", run `az login` again.
- To sign from a CI runner instead of an interactive login, create a
  service principal with the same role and either export
  `AZURE_CLIENT_ID` / `AZURE_TENANT_ID` / `AZURE_CLIENT_SECRET` for
  `az login --service-principal`, or pre-fetch a token and pass it as
  `ZULU_SIGN_TOKEN`.
- Signing changes the bytes of `generalszh_zulu.exe`, so the dev channel's
  `exe_sha256` gate still works: the Makefile hashes the staged exe after
  signing.
