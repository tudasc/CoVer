import xml.etree.ElementTree as ET
import re
import sys

def enrich_lit_xunit(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()
    
    # Find all <testcase> nodes
    for testcase in root.findall('.//testcase'):
        # Use the XML parser to get the <failure> child node
        failure = testcase.find('failure')
        
        if failure is not None:
            output = testcase.find('system-out').text

            # First, separate system-out into stdout/stderr
            output_redir = {
                "stdout": "system-out",
                "stderr": "system-err",
            }
            for stream in output_redir.keys():
                stream_out = re.search(rf"Command Output \({stream}\):\n--\n(.*?)\n--\n", output, re.M | re.DOTALL)
                if stream_out:
                    prev_stream = testcase.find(output_redir[stream])
                    if prev_stream is not None: testcase.remove(prev_stream)
                    subelem = ET.SubElement(testcase, output_redir[stream])
                    subelem.text = stream_out.group(1)

            # Find failing CHECK line
            line_match = re.search(r'(Tests/.+):(\d+):(\d+): error: CHECK:', output)

            if line_match:
                file = line_match.group(1)
                line = line_match.group(2)
                column = line_match.group(3)

                testcase.set('line', line)
                testcase.set('file', file)
            
    # Write the modified XML in-place
    tree.write(xml_path, encoding="UTF-8", xml_declaration=True)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python enrich_xunit.py <results.xml>")
        sys.exit(1)
    enrich_lit_xunit(sys.argv[1])
