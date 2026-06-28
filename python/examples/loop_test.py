import time

from ble_at import BleAtHost


TARGET_MAC = "AA:BB:CC:DD:EE:FF"
TARGET_ADDR_TYPE = 0


def main() -> None:
    ble = BleAtHost("COM15")
    try:
        while True:
            ble.connect(TARGET_MAC, TARGET_ADDR_TYPE)
            ble.send(b"PING")
            print(ble.get_status())
            print(ble.get_stream_stat())
            ble.disconnect()
            time.sleep(1.0)
    finally:
        ble.close()


if __name__ == "__main__":
    main()
