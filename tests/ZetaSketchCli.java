import com.google.zetasketch.HyperLogLogPlusPlus;
import java.util.Base64;
import java.util.Scanner;

public class ZetaSketchCli {
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
    }
  }
}
