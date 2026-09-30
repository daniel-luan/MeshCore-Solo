import { convert } from "./format.mjs";
self.onmessage = ({ data }) => {
  try {
    const route = convert(data.path, data.waypoints, data.options);
    self.postMessage({ id: data.id, route }, [route.data.buffer]);
  } catch (e) {
    self.postMessage({ id: data.id, error: e.message });
  }
};
