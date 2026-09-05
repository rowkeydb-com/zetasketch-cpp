import static java.nio.charset.StandardCharsets.UTF_8;

import com.google.zetasketch.HyperLogLogPlusPlus;
import com.google.zetasketch.internal.hllplus.Data;
import java.util.Base64;
import java.util.Scanner;

public class ZetaSketchCli {
  // Decodes hexadecimal strictly. The length must be even and every
  // character a hexadecimal digit; a trailing nibble is refused rather
  // than dropped, and a sign is refused rather than accepted, because a
  // lenient reading would merge an operand the script did not name and
  // report nothing.
  private static String hex(byte[] bytes) {
    StringBuilder sb = new StringBuilder();
    for (byte b : bytes) {
      sb.append(String.format("%02x", b));
    }
    return sb.toString();
  }

  private static byte[] parseHex(String text) {
    if (text.length() % 2 != 0) {
      throw new IllegalArgumentException(
          "hexadecimal of odd length " + text.length());
    }
    byte[] bytes = new byte[text.length() / 2];
    for (int i = 0; i < bytes.length; i++) {
      int high = Character.digit(text.charAt(2 * i), 16);
      int low = Character.digit(text.charAt(2 * i + 1), 16);
      if (high < 0 || low < 0) {
        throw new IllegalArgumentException(
            "not hexadecimal at offset " + (2 * i) + ": " + text);
      }
      bytes[i] = (byte) ((high << 4) | low);
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
    } else if ("CREATE_BATCH".equals(mode)) {
      // Builds many sketches in one invocation. Each block begins with
      // a SKETCH line carrying the two precisions and is followed by
      // its values, one per line and encoded so that no value can be
      // mistaken for a header. One line of output is written per
      // block, in order. Starting a virtual machine costs far more
      // than building a sketch, so batching the blocks that a
      // comparison needs turns hundreds of starts into one.
      Scanner scanner = new Scanner(System.in);
      HyperLogLogPlusPlus<String> hll = null;
      StringBuilder out = new StringBuilder();
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine();
        if (line.isEmpty()) {
          continue;
        }
        if (line.startsWith("SKETCH ")) {
          if (hll != null) {
            out.append(hex(hll.serializeToByteArray())).append('\n');
          }
          String[] parts = line.split(" ");
          int normalPrecision = Integer.parseInt(parts[1]);
          int sparsePrecision = Integer.parseInt(parts[2]);
          HyperLogLogPlusPlus.Builder builder =
              new HyperLogLogPlusPlus.Builder().normalPrecision(normalPrecision);
          if (sparsePrecision == 0) {
            builder.noSparseMode();
          } else {
            builder.sparsePrecision(sparsePrecision);
          }
          hll = builder.buildForStrings();
        } else if (line.startsWith("ITEM ")) {
          if (hll == null) {
            throw new IllegalStateException("ITEM before SKETCH");
          }
          hll.add(new String(Base64.getDecoder().decode(line.substring(5)), UTF_8));
        } else {
          throw new IllegalStateException("unexpected line: " + line);
        }
      }
      if (hll != null) {
        out.append(hex(hll.serializeToByteArray())).append('\n');
      }
      System.out.print(out);
    } else if ("MERGE_BATCH".equals(mode)) {
      // Merges many groups of sketches in one invocation. Each block
      // begins with a MERGE line and is followed by its operands as
      // hexadecimal, which cannot be mistaken for the header. One line
      // of output is written per block, in order.
      Scanner scanner = new Scanner(System.in);
      HyperLogLogPlusPlus<String> hll = null;
      boolean inBlock = false;
      StringBuilder out = new StringBuilder();
      while (scanner.hasNextLine()) {
        String line = scanner.nextLine().trim();
        if (line.isEmpty()) {
          continue;
        }
        if ("MERGE".equals(line)) {
          if (inBlock) {
            out.append(hll == null ? "" : hex(hll.serializeToByteArray())).append('\n');
          }
          hll = null;
          inBlock = true;
        } else {
          if (!inBlock) {
            throw new IllegalStateException("operand before MERGE");
          }
          @SuppressWarnings("unchecked")
          HyperLogLogPlusPlus<String> other =
              (HyperLogLogPlusPlus<String>) HyperLogLogPlusPlus.forProto(parseHex(line));
          if (hll == null) {
            hll = other;
          } else {
            hll.merge(other);
          }
        }
      }
      if (inBlock) {
        out.append(hll == null ? "" : hex(hll.serializeToByteArray())).append('\n');
      }
      System.out.print(out);
    } else if ("SCRIPT".equals(mode)) {
      // Applies a sequence of commands to one sketch, so that a
      // comparison can reach the states an operation leaves behind for
      // the next one rather than only the state a parse produces. The
      // builder is chosen by the type argument, as the reference fixes
      // an aggregator's value type when it is built.
      String type = args[1];
      int normalPrecision = Integer.parseInt(args[2]);
      int sparsePrecision = Integer.parseInt(args[3]);

      HyperLogLogPlusPlus.Builder builder =
          new HyperLogLogPlusPlus.Builder().normalPrecision(normalPrecision);
      if (sparsePrecision == 0) {
        builder.noSparseMode();
      } else {
        builder.sparsePrecision(sparsePrecision);
      }
      // The builders for text and for byte arrays are the same builder
      // in the reference: both record the value type shared by strings
      // and byte arrays, and the generic parameter that distinguishes
      // them is erased here. A comparison between the two would compare
      // an aggregator with itself.
      HyperLogLogPlusPlus<?> built;
      if ("strings".equals(type)) {
        built = builder.buildForStrings();
      } else if ("bytes".equals(type)) {
        built = builder.buildForBytes();
      } else if ("longs".equals(type)) {
        built = builder.buildForLongs();
      } else {
        // A type that is not recognised must not fall back to any
        // builder. A strings aggregator accepts both text and byte
        // values, so a mistyped script would otherwise run to
        // completion and print checkpoints that look plausible while
        // measuring an aggregator nobody asked for.
        System.out.println("BADINPUT unknown type " + type);
        return;
      }
      @SuppressWarnings("unchecked")
      HyperLogLogPlusPlus<Object> hll = (HyperLogLogPlusPlus<Object>) built;

      Scanner scanner = new Scanner(System.in);
      while (scanner.hasNextLine()) {
        // Only a carriage return is stripped, never a trailing
        // space: the space after a command is the separator, and
        // the argument that follows it may legitimately be empty.
        String line = scanner.nextLine();
        while (!line.isEmpty()
            && (line.charAt(line.length() - 1) == '\r')) {
          line = line.substring(0, line.length() - 1);
        }
        if (line.isEmpty()) {
          continue;
        }
        String[] parts = line.split(" ", 2);
        String command = parts[0];
        // A line with no separator carries no argument; a line with one
        // may carry an empty argument, which for an addition is the
        // empty value and is a distinct input the reference accepts.
        boolean hasArgument = parts.length > 1;
        String argument = hasArgument ? parts[1] : "";

        // The argument is read before the library is called, and a
        // failure to read one carries its own marker. A script the
        // harness cannot parse is a fault in the script; a value the
        // reference declines is a fact about the reference. Reporting
        // both as the same thing would let a broken script read as a
        // pinned refusal.
        byte[] payload = null;
        long longArgument = 0;
        try {
          boolean takesArgument =
              "ADD_STRING".equals(command)
                  || "ADD_BYTES".equals(command)
                  || "ADD_LONG".equals(command)
                  || "MERGE".equals(command);
          if (takesArgument && !hasArgument) {
            throw new IllegalArgumentException("missing argument for " + command);
          }
          if ("ADD_STRING".equals(command) || "ADD_BYTES".equals(command)) {
            payload = Base64.getDecoder().decode(argument);
          } else if ("ADD_LONG".equals(command)) {
            longArgument = Long.parseLong(argument);
          } else if ("MERGE".equals(command)) {
            // An operand of no bytes cannot reach here: an argument
            // that is absent is refused above, and hexadecimal that is
            // present decodes to at least one byte or is refused as
            // being of odd length.
            payload = parseHex(argument);
          }
        } catch (RuntimeException e) {
          System.out.println("BADINPUT " + e.getMessage());
          continue;
        }

        // A command the reference refuses reports itself and the script
        // continues, so that a refusal is as observable as a result.
        try {
          if ("ADD_STRING".equals(command)) {
            // A text channel, not a byte channel: the reference hashes
            // a string by encoding it as UTF-8, so a value that is not
            // valid UTF-8 is replaced before it is hashed. Use
            // ADD_BYTES to hash arbitrary bytes exactly.
            hll.add(new String(payload, UTF_8));
          } else if ("ADD_BYTES".equals(command)) {
            hll.add(payload);
          } else if ("ADD_LONG".equals(command)) {
            hll.add(longArgument);
          } else if ("MERGE".equals(command)) {
            hll.merge(payload);
          } else if ("CHECKPOINT".equals(command)) {
            StringBuilder sb = new StringBuilder();
            for (byte b : hll.serializeToByteArray()) {
              sb.append(String.format("%02x", b));
            }
            System.out.println(sb);
          } else if ("RESULT".equals(command)) {
            System.out.println(hll.result());
          } else {
            System.out.println("BADINPUT unknown command " + command);
          }
        } catch (RuntimeException e) {
          System.out.println("ERROR " + e.getMessage());
        }
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
