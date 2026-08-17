import os
os.environ['VITIS_EMBEDDED_INSTALL'] = 'true'
import vitis


# create_platform.py と同じ理由でsubst済みの短いドライブレターを使う
# (`subst W: <REPO_ROOT>\projects\poly_mul_ntt_crt_n8192_soc`)。
PROJ_ROOT = "W:"
WORKSPACE = PROJ_ROOT + "/software/workspace"
PLATFORM_XPFM = WORKSPACE + "/crt8192_platform/export/crt8192_platform/crt8192_platform.xpfm"
SRC_DIR = PROJ_ROOT + "/software/src"
DOMAIN_NAME = "standalone_domain"
APP_NAME = "poly_mul_crt_n8192_app"

client = vitis.create_client(workspace=WORKSPACE)

app = client.create_app_component(
    name=APP_NAME,
    platform=PLATFORM_XPFM,
    domain=DOMAIN_NAME,
    template="empty_application",
)

# main.c を src/ に配置 (テンプレートが作る既定のsrcにあるサンプルファイルは残っていても
# 上書きされないので、事前に生成された既定ソースがある場合はそちらを削除してから追加する)
app.import_files(from_loc=SRC_DIR, files=["main.c"], dest_dir_in_cmp="src")

status = app.build()
print("APP_BUILD_STATUS:", status)

vitis.dispose()
