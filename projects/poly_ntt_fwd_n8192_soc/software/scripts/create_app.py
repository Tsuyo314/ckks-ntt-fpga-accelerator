import os
os.environ['VITIS_EMBEDDED_INSTALL'] = 'true'
import vitis


# create_platform.py と同じ理由でsubst済みの短いドライブレターを使う
# (`subst W: <REPO_ROOT>\projects\poly_ntt_fwd_n8192_soc`)。
PROJ_ROOT = "W:"
WORKSPACE = PROJ_ROOT + "/software/workspace"
PLATFORM_XPFM = WORKSPACE + "/fwd8192_platform/export/fwd8192_platform/fwd8192_platform.xpfm"
SRC_DIR = PROJ_ROOT + "/software/src"
DOMAIN_NAME = "standalone_domain"
APP_NAME = "poly_ntt_fwd_app"

client = vitis.create_client(workspace=WORKSPACE)

app = client.create_app_component(
    name=APP_NAME,
    platform=PLATFORM_XPFM,
    domain=DOMAIN_NAME,
    template="empty_application",
)

app.import_files(from_loc=SRC_DIR, files=["main.c"], dest_dir_in_cmp="src")

status = app.build()
print("APP_BUILD_STATUS:", status)

vitis.dispose()
