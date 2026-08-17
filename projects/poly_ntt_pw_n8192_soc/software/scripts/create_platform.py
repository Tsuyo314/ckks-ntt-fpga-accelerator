import os
os.environ['VITIS_EMBEDDED_INSTALL'] = 'true'
import vitis


# Windows MAX_PATH(260バイト)対策: BSPビルド時の.obj.d依存ファイルパスが260バイトを
# 超えて失敗するため、プロジェクトルートを subst で短いドライブレターにマッピングして
# 実行する (`subst W: <REPO_ROOT>\projects\poly_ntt_pw_n8192_soc`)。
PROJ_ROOT = "W:"
XSA = PROJ_ROOT + "/low_level/pw8192.runs/impl_1/design_1_wrapper.xsa"
WORKSPACE = PROJ_ROOT + "/software/workspace"
PLATFORM_NAME = "pw8192_platform"  # Windows MAX_PATH(260)対策で短縮
DOMAIN_NAME = "standalone_domain"

client = vitis.create_client(workspace=WORKSPACE)

platform = client.create_platform_component(
    name=PLATFORM_NAME,
    hw_design=XSA,
    os="standalone",
    cpu="microblaze_0",
    domain_name=DOMAIN_NAME,
)

domain = platform.get_domain(DOMAIN_NAME)
domain.set_config(option="os", param="standalone_stdin", value="axi_uartlite_0")
domain.set_config(option="os", param="standalone_stdout", value="axi_uartlite_0")

status = platform.build()
print("PLATFORM_BUILD_STATUS:", status)

vitis.dispose()
