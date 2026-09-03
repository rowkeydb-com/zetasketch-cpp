import com.google.zetasketch.HyperLogLogPlusPlus;
import com.google.zetasketch.internal.hllplus.Data;
import java.util.Base64;
import java.util.Scanner;

public class ZetaSketchCli {
  private static byte[] parseHex(String text) {
    byte[] bytes = new byte[text.length() / 2];
    for (int i = 0; i < bytes.length; i++) {
      bytes[i] = (byte) Integer.parseInt(text.substring(2 * i, 2 * i + 2), 16);
    }
    return bytes;
  }

  public static void main(String[] args) throws Exception {
    if (args.length == 0) {
      System.err.println("Usage: ZetaSketchCli <CREATE|MERGE> [normal_precision sparse_precision]");
      System.exit(1);
    }

    String mode = args[0];
    if ("CREATE".equals(mode)) {
      int normalPrecision = Integer.parseInt(args[1]);
      int sparsePrecision = Integer.parseInt(args[2]);

      HyperLogLogPlusPlus.Builder builder =
          new HyperLogLogPlusPlus.Builder().normalPrecision(normalPrecision);
      if (sparsePrecision == 0) {
        builder.noSparseMode();
      } else {
        builder.sparsePrecision(sparsePrecision);
      }

      HyperLogLogPlusPlus<String> hll = builder.buildForStrings();

      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (!line.isEmpty()) {
          hll.add(line);
        }
      }
      byte[] serialized = hll.serializeToByteArray();
      StringBuilder sb = new StringBuilder();
      for (byte b : serialized) {
        sb.append(String.format("%02x", b));
      }
      System.out.println(sb.toString());
    } else if ("ESTIMATE".equals(mode)) {
      int normalPrecision = Integer.parseInt(args[1]);
      int sparsePrecision = Integer.parseInt(args[2]);

      HyperLogLogPlusPlus.Builder builder =
          new HyperLogLogPlusPlus.Builder().normalPrecision(normalPrecision);
      if (sparsePrecision == 0) {
        builder.noSparseMode();
      } else {
        builder.sparsePrecision(sparsePrecision);
      }

      HyperLogLogPlusPlus<String> hll = builder.buildForStrings();

      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (!line.isEmpty()) {
          hll.add(line);
        }
      }
      System.out.println(hll.result());
    } else if ("ESTIMATE_SWEEP".equals(mode)) {
      int normalPrecision = Integer.parseInt(args[1]);
      int sparsePrecision = Integer.parseInt(args[2]);

      HyperLogLogPlusPlus.Builder builder =
          new HyperLogLogPlusPlus.Builder().normalPrecision(normalPrecision);
      if (sparsePrecision == 0) {
        builder.noSparseMode();
      } else {
        builder.sparsePrecision(sparsePrecision);
      }

      HyperLogLogPlusPlus<String> hll = builder.buildForStrings();

      // Reports the estimate after every addition, so that a caller
      // can observe where the estimator changes.
      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (!line.isEmpty()) {
          hll.add(line);
          System.out.println(hll.result());
        }
      }
    } else if ("ESTIMATE_BIAS".equals(mode)) {
      // Reads an estimate and a precision per line and reports the
      // bias correction the reference computes for them.
      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine().trim();
        if (line.isEmpty()) {
          continue;
        }
        String[] parts = line.split(" ");
        System.out.println(
            Data.estimateBias(Double.parseDouble(parts[0]), Integer.parseInt(parts[1])));
      }
    } else if ("ESTIMATE_BYTES".equals(mode)) {
      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine().trim();
        if (line.isEmpty()) {
          continue;
        }
        byte[] bytes = new byte[line.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
          bytes[i] = (byte) Integer.parseInt(line.substring(2 * i, 2 * i + 2), 16);
        }
        System.out.println(HyperLogLogPlusPlus.forProto(bytes).result());
      }
    } else if ("MERGE".equals(mode)) {
      Scanner scanner = new Scanner(System.in);
      HyperLogLogPlusPlus<String> hll = null;
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (line.isEmpty())
          continue;
        // Parse hex
        byte[] bytes = new byte[line.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
          bytes[i] = (byte) Integer.parseInt(line.substring(2 * i, 2 * i + 2), 16);
        }
        HyperLogLogPlusPlus<String> other =
            (HyperLogLogPlusPlus<String>) HyperLogLogPlusPlus.forProto(bytes);
        if (hll == null) {
          hll = other;
        } else {
          hll.merge(other);
        }
      }
      if (hll != null) {
        byte[] serialized = hll.serializeToByteArray();
        StringBuilder sb = new StringBuilder();
        for (byte b : serialized) {
          sb.append(String.format("%02x", b));
        }
        System.out.println(sb.toString());
      }
    } else if ("TRANSITION".equals(mode)) {
      // Each input line is an operation, a serialised sketch in
      // hexadecimal, and an argument, separated by tabs. The operation
      // is applied to the parsed sketch and the outcome reported: the
      // verdict, the cardinality, and the bytes the library then
      // writes. This is what the state-transition comparisons are
      // measured against, the parse-then-serialise comparison alone
      // never reaching an operation.
      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (line.trim().isEmpty()) {
          continue;
        }
        String[] fields = line.split("\t", -1);
        String operation = fields[0];
        byte[] bytes = parseHex(fields[1]);
        String argument = fields.length > 2 ? fields[2] : "";
        try {
          @SuppressWarnings("unchecked")
          HyperLogLogPlusPlus<Object> hll =
              (HyperLogLogPlusPlus<Object>) HyperLogLogPlusPlus.forProto(bytes);
          if ("ADD".equals(operation)) {
            String[] parts = argument.split(":", 2);
            int count = Integer.parseInt(parts[0]);
            for (int i = 0; i < count; i++) {
              hll.add(parts[1] + i);
            }
          } else if ("ADD_AND_WRITE".equals(operation)) {
            // Adds, then writes the sketch out and discards the bytes.
            // Writing compacts, which can promote a sparse sketch, so
            // the cardinality reported afterwards is not the one an
            // unwritten sketch would report.
            String[] parts = argument.split(":", 2);
            int count = Integer.parseInt(parts[0]);
            for (int i = 0; i < count; i++) {
              hll.add(parts[1] + i);
            }
            byte[] discarded = hll.serializeToByteArray();
            if (discarded.length == 0) {
              throw new IllegalStateException("empty serialization");
            }
          } else if ("MERGE".equals(operation)) {
            @SuppressWarnings("unchecked")
            HyperLogLogPlusPlus<Object> other =
                (HyperLogLogPlusPlus<Object>) HyperLogLogPlusPlus.forProto(parseHex(argument));
            hll.merge(other);
          } else if (!"RESULT".equals(operation)) {
            throw new IllegalArgumentException("unknown operation " + operation);
          }
          long result = hll.result();
          StringBuilder sb = new StringBuilder();
          for (byte b : hll.serializeToByteArray()) {
            sb.append(String.format("%02x", b));
          }
          System.out.println("ACCEPT\t" + result + "\t" + sb);
        } catch (RuntimeException e) {
          System.out.println("REFUSE\t\t\t" + e.getMessage());
        }
      }
    } else if ("ROUNDTRIP".equals(mode)) {
      // Each input line is a serialised sketch in hexadecimal. For each
      // one, report whether the library accepts it and, when it does,
      // what it writes back out. This is what the parse-shape and
      // round-trip comparisons are measured against.
      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine().trim();
        if (line.isEmpty()) {
          continue;
        }
        byte[] bytes = new byte[line.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
          bytes[i] = (byte) Integer.parseInt(line.substring(2 * i, 2 * i + 2), 16);
        }
        try {
          HyperLogLogPlusPlus<?> hll = HyperLogLogPlusPlus.forProto(bytes);
          StringBuilder sb = new StringBuilder();
          for (byte b : hll.serializeToByteArray()) {
            sb.append(String.format("%02x", b));
          }
          System.out.println("ACCEPT " + sb.toString());
        } catch (RuntimeException e) {
          System.out.println("REFUSE");
        }
      }
    }
  }
}
