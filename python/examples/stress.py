import os
import time

from ble_at import BleAtHost


PORT = os.environ.get("BLE_AT_PORT", "COM15")
TARGET_MAC = os.environ.get("BLE_AT_TARGET_MAC", "AA:BB:CC:DD:EE:FF")
TARGET_ADDR_TYPE = int(os.environ.get("BLE_AT_TARGET_ADDR_TYPE", "0"))
PAYLOAD = b"0123456789abcdef" * 32


def main() -> None:
    ble = BleAtHost(PORT)
    try:
        ble.connect(TARGET_MAC, TARGET_ADDR_TYPE)
        ble.enter_data_mode()
        start = time.time()
        while time.time() - start < 60.0:
            ble.write_stream(PAYLOAD)
            time.sleep(0.05)
        ble.exit_data_mode()
        print(ble.get_stream_stat())
        ble.disconnect()
    finally:
        ble.close()


if __name__ == "__main__":
    main()
